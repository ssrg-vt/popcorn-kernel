/**
 * @file page_server.c
 *
 * Popcorn Linux page server implementation
 * This work was an extension of Marina Sadini MS Thesis, but totally revamped
 * for multi-threaded setup.
 *
 * @author Sang-Hoon Kim, SSRG Virginia Tech 2017
 */

#include <linux/compiler.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/rmap.h>
#include <linux/mmu_notifier.h>
#include <linux/wait.h>
#include <linux/ptrace.h>
#include <linux/swap.h>
#include <linux/pagemap.h>
#include <linux/delay.h>
#include <linux/random.h>
#include <linux/radix-tree.h>
#include <linux/mman.h>

#include <asm/tlbflush.h>
#include <asm/cacheflush.h>
#include <asm/mmu_context.h>

#include <popcorn/types.h>
#include <popcorn/bundle.h>
#include <popcorn/pcn_kmsg.h>

#include "types.h"
#include "pgtable.h"
#include "wait_station.h"
#include "page_server.h"
#include "fh_action.h"

#include "trace_events.h"

//#define PFPRINTK(...) printk(__VA_ARGS__)
//#define PFPRINTK(...) printk(KERN_INFO __VA_ARGS__)
#define PFPRINTK(...)
#define PREFETCH_SUPPORT 1
#define PREFETCH_REMOTE_REMOTE_SUPPORT 1

#define PREFETCH_REMOTE_REMOTE_IMM 1

struct fault_handle {
	struct hlist_node list;

	unsigned long addr;
	unsigned long flags;

	unsigned int limit;
	pid_t pid;
	int ret;

	atomic_t pendings;
	atomic_t pendings_retry;
	wait_queue_head_t waits;
	wait_queue_head_t waits_retry;
	struct remote_context *rc;

	struct completion *complete;
#ifdef CONFIG_POPCORN_STAT
	struct timeval tv_start;
#endif
};


/* Ongoing prefetching requests mapping table per batched request */
struct pf_ongoing_map {
	struct list_head list;
	int pid;
	int pf_list_size;
	unsigned long addr[MAX_PF_REQ];
	struct fault_handle *fh[MAX_PF_REQ];
	struct pcn_kmsg_rdma_handle *rh[MAX_PF_REQ];
};

struct got_remote_page_prefetch_req {
	struct list_head list;
	unsigned long addr;
	unsigned long flags;
	bool is_write;
};

#ifdef CONFIG_POPCORN_STAT
enum pf_action_code {
	/* MADV */
	PF_READ_MADV = 0,
	PF_WRITE_MADV,
	NONE1,

	/* REQ */
	PF_READ_REQ = 3,	/* redundant */
	PF_WRITE_X_REQ,		/* redundant */
	PF_WRITE_O_REQ,		/* redundant */

	/* SUCC */
	PF_READ_RES_SUCCESS = 6,
	PF_WRITE_X_RES_SUCCCESS,
	PF_WRITE_O_RES_SUCCCESS,

	/* FAIL */
	PF_READ_RES_FAIL = 9,
	PF_WRITE_X_RES_FAIL,	// = PF_WRITE_RES_FAIL
	PF_WRITE_O_RES_FAIL,	// += '10'

	/* REMOTE FAIL */
	REMOTE_PF_FAIL_NO_PTE = 12,
	REMOTE_PF_FAIL_MINE,
	REMOTE_PF_FAIL_BUSY,

	/* ORIGIN FAIL */
	ORIGIN_PF_FAIL_BUSY = 15,
	ORIGIN_PF_FAIL_REMOTE,
	NONE2,

	/* ORIGIN REMTOE-REMOTE FAIL */
	ORIGIN_RR_FAIL_MINE = 18,
	ORIGIN_RR_FAIL_OWNER,
	ORIGIN_RR_FAIL_BUSY,

	/* RELEASE */
	PF_REALEASE_MADV = 21, /* redundant */
	PF_RELEASE_RES_SUCCESS,
	PF_RELEASE_RES_FAIL,

	PF_ACTION_TYPE_MAX
};
static unsigned long __pf_action_stat[PF_ACTION_TYPE_MAX] = { 0 };

const char *pf_action_type_name[PF_ACTION_TYPE_MAX] = {
    [PF_READ_MADV] = "MADV",
    [PF_READ_REQ] = "REQ (R)",				/* redundant */
	[PF_READ_RES_SUCCESS] = "SUCC",
	[PF_READ_RES_FAIL] = "FAIL (O)",
	[REMOTE_PF_FAIL_NO_PTE] = "FAIL R - !pte | mine | bz",
	[ORIGIN_PF_FAIL_BUSY] = "FAIL O - bz | remote | none",
	[ORIGIN_RR_FAIL_MINE] = "FAIL O RR - mine | owner | bz",
	[PF_REALEASE_MADV] = "RELEASE",
};
#endif

void pf_action_stat(struct seq_file *seq, void *v) {
#ifdef CONFIG_POPCORN_STAT
    int i, k = 3;
	if (seq)
		seq_printf(seq, "%2s  %12s   %2s  %12s   %2s  %12s\n",
							"", "R", "", "WX", "", "WO");
    for (i = 0; i < ARRAY_SIZE(__pf_action_stat) / k; i++) {
        if (seq) {
            seq_printf(seq, "%2d  %12lu   %2d  %12lu   %2d  %12lu   %s\n",
							i*k + 0, __pf_action_stat[i*k + 0],
							i*k + 1, __pf_action_stat[i*k + 1],
							i*k + 2, __pf_action_stat[i*k + 2],
							pf_action_type_name[i*k + 0]);
        } else {
            __pf_action_stat[i*k + 0] = 0;
            __pf_action_stat[i*k + 1] = 0;
            __pf_action_stat[i*k + 2] = 0;
        }
    }
#endif
}

inline void pf_action_record_remote_res_fail_read(void)
{
#ifdef CONFIG_POPCORN_STAT
	__pf_action_stat[PF_READ_RES_FAIL]++;
#endif
}

inline void pf_action_record_remote_res_fail_write(void)
{
#ifdef CONFIG_POPCORN_STAT
	__pf_action_stat[PF_WRITE_X_RES_FAIL]++;
	__pf_action_stat[PF_WRITE_O_RES_FAIL]++;
#endif
}



inline void pf_action_record_remote_res_succ_read(void)
{
#ifdef CONFIG_POPCORN_STAT
	__pf_action_stat[PF_READ_RES_SUCCESS]++;
#endif
}

inline void pf_action_record_remote_res_succ_write_not_grant(void)
{
#ifdef CONFIG_POPCORN_STAT
	__pf_action_stat[PF_WRITE_X_RES_SUCCCESS]++;
#endif
}

inline void pf_action_record_remote_res_succ_write_grant(void)
{
#ifdef CONFIG_POPCORN_STAT
	__pf_action_stat[PF_WRITE_O_RES_SUCCCESS]++;
#endif
}


inline void pf_action_record_remote_madv_read(void)
{
#ifdef CONFIG_POPCORN_STAT
	__pf_action_stat[PF_READ_MADV]++;
#endif
}

inline void pf_action_record_remote_madv_write(void)
{
#ifdef CONFIG_POPCORN_STAT
	__pf_action_stat[PF_WRITE_MADV]++;
#endif
}

inline void pf_action_record_remote_req_read(void)
{
#ifdef CONFIG_POPCORN_STAT
	__pf_action_stat[PF_READ_REQ]++;
#endif
}

inline void pf_action_record_remote_req_write_not_grant(void)
{
#ifdef CONFIG_POPCORN_STAT
	__pf_action_stat[PF_WRITE_X_REQ]++;
#endif
}

inline void pf_action_record_remote_req_write_grant(void)
{
#ifdef CONFIG_POPCORN_STAT
	__pf_action_stat[PF_WRITE_O_REQ]++;
#endif
}


inline void pf_action_fail_record_origin_busy(void)
{
#ifdef CONFIG_POPCORN_STAT
	__pf_action_stat[ORIGIN_PF_FAIL_BUSY]++;
#endif
}

inline void pf_action_fail_record_origin_pg_not_mine(void)
{
#ifdef CONFIG_POPCORN_STAT
	__pf_action_stat[ORIGIN_PF_FAIL_REMOTE]++; /* remote remote */
#endif
}

inline void pf_action_fail_record_remote_no_pte(void)
{
#ifdef CONFIG_POPCORN_STAT
	__pf_action_stat[REMOTE_PF_FAIL_NO_PTE]++;
#endif
}

inline void pf_action_fail_record_remote_pg_is_mine(void)
{
#ifdef CONFIG_POPCORN_STAT
	__pf_action_stat[REMOTE_PF_FAIL_MINE]++;
#endif
}

inline void pf_action_fail_record_remote_busy(void)
{
#ifdef CONFIG_POPCORN_STAT
	__pf_action_stat[REMOTE_PF_FAIL_BUSY]++;
#endif
}


inline void pfpf_fail_action_record(bool mine, bool owner, bool busy)
{
#ifdef CONFIG_POPCORN_STAT
	if (mine)
		__pf_action_stat[ORIGIN_RR_FAIL_MINE]++;
	else if (owner)
		__pf_action_stat[ORIGIN_RR_FAIL_OWNER]++;
	else if (busy)
		__pf_action_stat[ORIGIN_RR_FAIL_BUSY]++;
	else BUG();
#endif
}

/* for pf release */
inline void pf_action_release_record(bool req, bool succ)
{
#ifdef CONFIG_POPCORN_STAT
	if (req) {
		__pf_action_stat[PF_REALEASE_MADV]++;
	} else {
		if (succ)
			__pf_action_stat[PF_RELEASE_RES_SUCCESS]++;
		else
			__pf_action_stat[PF_RELEASE_RES_FAIL]++;
	}
#endif
}

#ifdef CONFIG_POPCORN_STAT
atomic64_t pf_cnt;
atomic64_t pf_time_u;

atomic64_t pf_fail_cnt;
atomic64_t pf_fail_time_u;
atomic64_t pf_succ_cnt;
atomic64_t pf_succ_time_u;
#endif
#ifdef CONFIG_POPCORN_STAT_PGFAULTS
atomic64_t mm_cnt;
atomic64_t mm_time_u;
#endif

/* track time by fh for recording pf time on remote sides */
inline void pf_time_start_with_fh(struct fault_handle *fh)
{
#ifdef CONFIG_POPCORN_STAT
	do_gettimeofday(&fh->tv_start);
	atomic64_inc(&pf_cnt);
#endif
}

inline void pf_time_end_with_fh(struct fault_handle *fh, bool succ)
{
#ifdef CONFIG_POPCORN_STAT
	unsigned long dt;
	struct timeval tv_end;
	do_gettimeofday(&tv_end);
	dt = ((tv_end.tv_sec * 1000000) + tv_end.tv_usec)
		- (fh->tv_start.tv_sec * 1000000)
		- fh->tv_start.tv_usec;
	if (dt < 0) BUG();
	atomic64_add(dt, &pf_time_u);

	if (succ) {
		atomic64_add(dt, &pf_succ_time_u);
		atomic64_inc(&pf_succ_cnt);
	} else {
		atomic64_add(dt, &pf_fail_time_u);
		atomic64_inc(&pf_fail_cnt);
	}
#endif
}

/* for recording pf time on remote sides */
inline void pf_time_start_at_remote(struct timeval* tv_start)
{
#ifdef CONFIG_POPCORN_STAT
	do_gettimeofday(tv_start);
#endif
}

inline void pf_time_end_at_remote(struct timeval* tv_start)
{
#ifdef CONFIG_POPCORN_STAT
	unsigned long dt;
	struct timeval tv_end;
	do_gettimeofday(&tv_end);
	dt = ((tv_end.tv_sec * 1000000) + tv_end.tv_usec)
		- (tv_start->tv_sec * 1000000)
		- tv_start->tv_usec;
	atomic64_add(dt, &pf_time_u);
#endif
}

/* for recording pf time on remote sides */
inline void pf_time_start_at_origin(struct timeval* tv_start)
{
#ifdef CONFIG_POPCORN_STAT
	do_gettimeofday(tv_start);
#endif
}

inline void pf_time_end_at_origin(struct timeval* tv_start)
{
#ifdef CONFIG_POPCORN_STAT
	unsigned long dt;
	struct timeval tv_end;
	do_gettimeofday(&tv_end);
	dt = ((tv_end.tv_sec * 1000000) + tv_end.tv_usec)
		- (tv_start->tv_sec * 1000000)
		- tv_start->tv_usec;
	atomic64_add(dt, &pf_time_u);
#endif
}

#define MILLISECOND 1000000
extern void pf_time_stat(struct seq_file *seq, void *v);
void pf_time_stat(struct seq_file *seq, void *v)
{
#ifdef CONFIG_POPCORN_STAT
	if (seq) {
		seq_printf(seq, "%2s  %5llu,%06llu   %3s %-12llu   %2s  %-12s\n",
						"mm", (u64)(atomic64_read(&mm_time_u) / MILLISECOND),
								(u64)(atomic64_read(&mm_time_u) % MILLISECOND),
						"per", atomic64_read(&mm_cnt) ? (u64)atomic64_read(&mm_time_u) / atomic64_read(&mm_cnt) : 0, "us", "(unit)");
		seq_printf(seq, "%2s  %5llu,%06llu   %3s %-12llu   %2s  %-12llu   %2s  %-12llu\n",
						"pf", (u64)(atomic64_read(&pf_time_u) / MILLISECOND),
								(u64)(atomic64_read(&pf_time_u) % MILLISECOND),
						"per",atomic64_read(&pf_cnt) ? (u64)atomic64_read(&pf_time_u) / atomic64_read(&pf_cnt) : 0,
						"-o", atomic64_read(&pf_succ_cnt) ? (u64)atomic64_read(&pf_succ_time_u) / atomic64_read(&pf_succ_cnt) : 0,
						"-x", atomic64_read(&pf_fail_cnt) ? (u64)atomic64_read(&pf_fail_time_u) / atomic64_read(&pf_fail_cnt) : 0);

	} else {
		atomic64_set(&pf_cnt, 0);
		atomic64_set(&pf_time_u, 0);

		atomic64_set(&pf_fail_cnt, 0);
		atomic64_set(&pf_fail_time_u, 0);
		atomic64_set(&pf_succ_cnt, 0);
		atomic64_set(&pf_succ_time_u, 0);

		atomic64_set(&mm_cnt, 0);
		atomic64_set(&mm_time_u, 0);
	}
#endif
}

inline void page_server_start_mm_fault(unsigned long address)
{
#ifdef CONFIG_POPCORN_STAT_PGFAULTS
	if (!distributed_process(current)) return;
	if (current->fault_address == 0 ||
			current->fault_address != address) {
		current->fault_address = address;
		current->fault_retry = 0;
		current->fault_start = ktime_get();
		current->fault_address = address;
	}
#endif
}

inline int page_server_end_mm_fault(int ret)
{
#ifdef CONFIG_POPCORN_STAT_PGFAULTS
	if (!distributed_process(current)) return ret;

	if (ret & VM_FAULT_RETRY) {
		current->fault_retry++;
	} else if (!(ret & VM_FAULT_ERROR)) {
		dt = ktime_sub(fault_end, current->fault_start);
		trace_pgfault_stat(instruction_pointer(current_pt_regs()),
				current->fault_address, ret,
				current->fault_retry, ktime_to_ns(dt));
		current->fault_address = 0;
		atomic64_add(dt, &mm_time_u);
		atomic64_inc(&mm_cnt);
	}
#endif
	return ret;
}


static inline int __fault_hash_key(unsigned long address)
{
	return (address >> PAGE_SHIFT) % FAULTS_HASH;
}

/**************************************************************************
 * Page ownership tracking mechanism
 */
#define PER_PAGE_INFO_SIZE \
		(sizeof(unsigned long) * BITS_TO_LONGS(MAX_POPCORN_NODES))
#define PAGE_INFO_PER_REGION (PAGE_SIZE / PER_PAGE_INFO_SIZE)

static inline void __get_page_info_key(unsigned long addr, unsigned long *key, unsigned long *offset)
{
	unsigned long paddr = addr >> PAGE_SHIFT;
	*key = paddr / PAGE_INFO_PER_REGION;
	*offset = (paddr % PAGE_INFO_PER_REGION) *
			(PER_PAGE_INFO_SIZE / sizeof(unsigned long));
}

static inline struct page *__get_page_info_page(struct mm_struct *mm, unsigned long addr, unsigned long *offset)
{
	unsigned long key;
	struct page *page;
	struct remote_context *rc = mm->remote;
	__get_page_info_key(addr, &key, offset);

	page = radix_tree_lookup(&rc->pages, key);
	if (!page) return NULL;

	return page;
}

static inline unsigned long *__get_page_info_mapped(struct mm_struct *mm, unsigned long addr, unsigned long *offset)
{
	unsigned long key;
	struct page *page;
	struct remote_context *rc = mm->remote;
	__get_page_info_key(addr, &key, offset);
	if (!rc) BUG();

	page = radix_tree_lookup(&rc->pages, key);
	if (!page) return NULL;

	return (unsigned long *)kmap_atomic(page) + *offset;
}

void free_remote_context_pages(struct remote_context *rc)
{
	int nr_pages;
	const int FREE_BATCH = 16;
	struct page *pages[FREE_BATCH];

	do {
		int i;
		nr_pages = radix_tree_gang_lookup(&rc->pages,
				(void **)pages, 0, FREE_BATCH);

		for (i = 0; i < nr_pages; i++) {
			struct page *page = pages[i];
			radix_tree_delete(&rc->pages, page_private(page));
			__free_page(page);
		}
	} while (nr_pages == FREE_BATCH);
}

#define PI_FLAG_COWED 62
#define PI_FLAG_DISTRIBUTED 63

static struct page *__lookup_page_info_page(struct remote_context *rc, unsigned long key)
{
	struct page *page = radix_tree_lookup(&rc->pages, key);
	if (!page) {
		int ret;
		page = alloc_page(GFP_ATOMIC | __GFP_ZERO);
		BUG_ON(!page);
		set_page_private(page, key);

		ret = radix_tree_insert(&rc->pages, key, page);
		BUG_ON(ret);
	}
	return page;
}

static inline void SetPageDistributed(struct mm_struct *mm, unsigned long addr)
{
	unsigned long key, offset;
	unsigned long *region;
	struct page *page;
	struct remote_context *rc = mm->remote;
	__get_page_info_key(addr, &key, &offset);

	page = __lookup_page_info_page(rc, key);
	region = kmap_atomic(page);
	set_bit(PI_FLAG_DISTRIBUTED, region + offset);
	kunmap_atomic(region);
}

static inline void SetPageCowed(struct mm_struct *mm, unsigned long addr)
{
	unsigned long key, offset;
	unsigned long *region;
	struct page *page;
	struct remote_context *rc = mm->remote;
	__get_page_info_key(addr, &key, &offset);

	page = __lookup_page_info_page(rc, key);
	region = kmap_atomic(page);
	set_bit(PI_FLAG_COWED, region + offset);
	kunmap_atomic(region);
}

static inline void ClearPageInfo(struct mm_struct *mm, unsigned long addr)
{
	unsigned long offset;
	unsigned long *pi = __get_page_info_mapped(mm, addr, &offset);

	if (!pi) return;
	clear_bit(PI_FLAG_DISTRIBUTED, pi);
	clear_bit(PI_FLAG_COWED, pi);
	bitmap_clear(pi, 0, MAX_POPCORN_NODES);
	kunmap_atomic(pi - offset);
}

static inline bool PageDistributed(struct mm_struct *mm, unsigned long addr)
{
	unsigned long offset;
	unsigned long *pi = __get_page_info_mapped(mm, addr, &offset);
	bool ret;

	if (!pi) return false;
	ret = test_bit(PI_FLAG_DISTRIBUTED, pi);
	kunmap_atomic(pi - offset);
	return ret;
}

static inline bool PageCowed(struct mm_struct *mm, unsigned long addr)
{
	unsigned long offset;
	unsigned long *pi = __get_page_info_mapped(mm, addr, &offset);
	bool ret;

	if (!pi) return false;
	ret = test_bit(PI_FLAG_COWED, pi);
	kunmap_atomic(pi - offset);
	return ret;
}

static inline bool page_is_mine(struct mm_struct *mm, unsigned long addr)
{
	unsigned long offset;
	unsigned long *pi = __get_page_info_mapped(mm, addr, &offset);
	bool ret = true;

	if (!pi) return true;
	if (!test_bit(PI_FLAG_DISTRIBUTED, pi)) goto out;
	ret = test_bit(my_nid, pi);
out:
	kunmap_atomic(pi - offset);
	return ret;
}

static inline bool test_page_owner(int nid, struct mm_struct *mm, unsigned long addr)
{
	unsigned long offset;
	unsigned long *pi = __get_page_info_mapped(mm, addr, &offset);
	bool ret;

	if (!pi) return false;
	ret = test_bit(nid, pi);
	kunmap_atomic(pi - offset);
	return ret;
}

static inline void set_page_owner(int nid, struct mm_struct *mm, unsigned long addr)
{
	unsigned long offset;
	unsigned long *pi = __get_page_info_mapped(mm, addr, &offset);
	set_bit(nid, pi);
	kunmap_atomic(pi - offset);
}

static inline void clear_page_owner(int nid, struct mm_struct *mm, unsigned long addr)
{
	unsigned long offset;
	unsigned long *pi = __get_page_info_mapped(mm, addr, &offset);
	if (!pi) return;

	clear_bit(nid, pi);
	kunmap_atomic(pi - offset);
}


/**************************************************************************
 * Fault tracking mechanism
 */
enum {
	FAULT_HANDLE_WRITE = 0x01,
	FAULT_HANDLE_INVALIDATE = 0x02,
	FAULT_HANDLE_REMOTE = 0x04,
	FAULT_HANDLE_RELEASE = 0x08,
};

static struct kmem_cache *__fault_handle_cache = NULL;


static struct fault_handle *__alloc_fault_handle(struct task_struct *tsk, unsigned long addr, struct remote_context *rc)
{
	struct fault_handle *fh =
			kmem_cache_alloc(__fault_handle_cache, GFP_ATOMIC);
	int fk = __fault_hash_key(addr);
	BUG_ON(!fh);

	INIT_HLIST_NODE(&fh->list);

	fh->addr = addr;
	fh->flags = 0;

	init_waitqueue_head(&fh->waits);
	init_waitqueue_head(&fh->waits_retry);
	atomic_set(&fh->pendings, 1);
	atomic_set(&fh->pendings_retry, 0);
	fh->limit = 0;
	fh->ret = 0;
	//fh->rc = get_task_remote(tsk);
	//fh->rc = __get_mm_remote(mm);
	fh->rc = rc;
	atomic_inc(&rc->count);
	fh->pid = tsk->pid;
	fh->complete = NULL;

	hlist_add_head(&fh->list, &fh->rc->faults[fk]);
	return fh;
}


static struct fault_handle *__start_invalidation(struct task_struct *tsk, unsigned long addr, spinlock_t *ptl)
{
	unsigned long flags;
	struct remote_context *rc = get_task_remote(tsk);
	struct fault_handle *fh;
	bool found = false;
	DECLARE_COMPLETION_ONSTACK(complete);
	int fk = __fault_hash_key(addr);

	spin_lock_irqsave(&rc->faults_lock[fk], flags); /* ask unlock(ptl) here not later */
	hlist_for_each_entry(fh, &rc->faults[fk], list) {
		if (fh->addr == addr) {
			PGPRINTK("  [%d] %s %s ongoing, wait\n", tsk->pid,
				fh->flags & FAULT_HANDLE_REMOTE ? "remote" : "local",
				fh->flags & FAULT_HANDLE_WRITE ? "write" : "read");
			BUG_ON(fh->flags & FAULT_HANDLE_INVALIDATE); // test this means coalesce
			fh->flags |= FAULT_HANDLE_INVALIDATE;
			fh->complete = &complete;
			found = true;
			/* TODO: coalse with RELEASE? or rety
					RELEASE doesn't colease with any */
			/* tips: leader will hold FAULT_HANDLE_RELEASE */
			/* tips: leader remember to wake fh->complete => should it inv agign?  */
			break;
		}
	}
	spin_unlock_irqrestore(&rc->faults_lock[fk], flags);
	put_task_remote(tsk);

	if (found) {
		spin_unlock(ptl);
		PGPRINTK(" +[%d] %lx %p\n", tsk->pid, addr, fh);
		wait_for_completion(&complete);
		PGPRINTK(" =[%d] %lx %p\n", tsk->pid, addr, fh);
		spin_lock(ptl);
	} else { /* pf: check: Being synchronized by origin no longer hold */
		fh = NULL;
		PGPRINTK(" =[%d] %lx\n", tsk->pid, addr);
	}
	return fh;
}

static void __finish_invalidation(struct fault_handle *fh)
{
	unsigned long flags;
	int fk;

	if (!fh) return;
	fk = __fault_hash_key(fh->addr);

	BUG_ON(atomic_read(&fh->pendings));
	spin_lock_irqsave(&fh->rc->faults_lock[fk], flags);
	hlist_del(&fh->list);
	spin_unlock_irqrestore(&fh->rc->faults_lock[fk], flags);

	__put_task_remote(fh->rc);
	if (atomic_read(&fh->pendings_retry)) {
		wake_up_all(&fh->waits_retry);
	} else {
		kmem_cache_free(__fault_handle_cache, fh);
	}
}

static struct fault_handle *__start_fault_handling(struct task_struct *tsk, unsigned long addr, unsigned long fault_flags, spinlock_t *ptl, bool *leader)
	__releases(ptl)
{
	unsigned long flags;
	struct fault_handle *fh;
	bool found = false;
	struct remote_context *rc = get_task_remote(tsk);
	DEFINE_WAIT(wait);
	int fk = __fault_hash_key(addr);

	spin_lock_irqsave(&rc->faults_lock[fk], flags);
	spin_unlock(ptl);

	hlist_for_each_entry(fh, &rc->faults[fk], list) {
		if (fh->addr == addr) {
			found = true;
			break;
		}
	}

	if (found) {
		unsigned long action =
				get_fh_action(tsk->at_remote, fh->flags, fault_flags);

		if (fh->flags & FAULT_HANDLE_RELEASE) {
			action = FH_ACTION_RETRY;
			action |= FH_ACTION_WAIT;
			PFPRINTK("a RELEASE op going on\n");
		}

#ifdef CONFIG_POPCORN_CHECK_SANITY
		BUG_ON(action == FH_ACTION_INVALID);
#endif

		if (action & FH_ACTION_RETRY) {
			if (action & FH_ACTION_WAIT) {
				goto out_wait_retry;
			}
			goto out_retry;
		}
#ifdef CONFIG_POPCORN_CHECK_SANITY
		BUG_ON(action != FH_ACTION_FOLLOW);
#endif

		if (fh->limit++ > FH_ACTION_MAX_FOLLOWER) {
			goto out_wait_retry;
		}

		atomic_inc(&fh->pendings);
#ifndef CONFIG_POPCORN_DEBUG_PAGE_SERVER
		prepare_to_wait(&fh->waits, &wait, TASK_UNINTERRUPTIBLE);
#else
		prepare_to_wait_exclusive(&fh->waits, &wait, TASK_UNINTERRUPTIBLE);
#endif
		spin_unlock_irqrestore(&rc->faults_lock[fk], flags);
		PGPRINTK(" +[%d] %lx %p\n", tsk->pid, addr, fh);
		put_task_remote(tsk);

		io_schedule();
		finish_wait(&fh->waits, &wait);

		fh->pid = tsk->pid;
		*leader = false;
		return fh;
	}

	fh = __alloc_fault_handle(tsk, addr, rc);
	fh->flags |= fault_for_write(fault_flags) ? FAULT_HANDLE_WRITE : 0;
	fh->flags |= (fault_flags & FAULT_FLAG_REMOTE) ? FAULT_HANDLE_REMOTE : 0;

	spin_unlock_irqrestore(&rc->faults_lock[fk], flags);
	put_task_remote(tsk);

	*leader = true;
	return fh;

out_wait_retry:
	atomic_inc(&fh->pendings_retry);
	prepare_to_wait(&fh->waits_retry, &wait, TASK_UNINTERRUPTIBLE);
	spin_unlock_irqrestore(&rc->faults_lock[fk], flags);
	put_task_remote(tsk);

	PGPRINTK("  [%d] waits %p\n", tsk->pid, fh);
	io_schedule();
	finish_wait(&fh->waits_retry, &wait);
	if (atomic_dec_and_test(&fh->pendings_retry)) {
		kmem_cache_free(__fault_handle_cache, fh);
	}
	return NULL;

out_retry:
	spin_unlock_irqrestore(&rc->faults_lock[fk], flags);
	put_task_remote(tsk);

	PGPRINTK("  [%d] locked. retry %p\n", tsk->pid, fh);
	return NULL;
}

static bool __finish_fault_handling(struct fault_handle *fh)
{
	unsigned long flags;
	bool last = false;
	int fk = __fault_hash_key(fh->addr);

	spin_lock_irqsave(&fh->rc->faults_lock[fk], flags);
	if (atomic_dec_return(&fh->pendings)) {
		PGPRINTK(" >[%d] %lx %p\n", fh->pid, fh->addr, fh);
#ifndef CONFIG_POPCORN_DEBUG_PAGE_SERVER
		wake_up_all(&fh->waits);
#else
		wake_up(&fh->waits);
#endif
	} else {
		PGPRINTK(">>[%d] %lx %p\n", fh->pid, fh->addr, fh);
		if (fh->complete) {
			complete(fh->complete); /* wake up invalidate -> __finish_inv */
		} else {
			hlist_del(&fh->list);
			last = true;
		}
	}
	spin_unlock_irqrestore(&fh->rc->faults_lock[fk], flags);

	if (last) {
		__put_task_remote(fh->rc);
		if (atomic_read(&fh->pendings_retry)) {
			wake_up_all(&fh->waits_retry);
		} else {
			kmem_cache_free(__fault_handle_cache, fh);
		}
	}
	return last;
}

#if 0
static struct fault_handle *__start_releasing(struct task_struct *tsk,
							unsigned long addr, spinlock_t *ptl, bool *leader)
{
	unsigned long flags;
	struct remote_context *rc = get_task_remote(tsk);
	struct fault_handle *fh = NULL;
	bool found = false;
	int fk = __fault_hash_key(addr);

	//rc = get_task_remote(tsk); //xxx
	fk = __fault_hash_key(addr);
	spin_lock_irqsave(&rc->faults_lock[fk], flags);
	spin_unlock(ptl); // xxx: check: removing this should be fine.

	hlist_for_each_entry(fh, &rc->faults[fk], list) {
		if (fh->addr == addr) {
			found = true;
			break;
		}
	}

	if (!found) {
		*leader = true;
		fh = __alloc_fault_handle(tsk, addr, rc); /* xxx coales inv req */
		fh->flags = FAULT_HANDLE_RELEASE;
	}
	spin_unlock_irqrestore(&rc->faults_lock[fk], flags);

	put_task_remote(tsk);
	return fh;
}
#endif
/**************************************************************************
 * Helper functions for PTE following
 */
static pte_t *__get_pte_at(struct mm_struct *mm, unsigned long addr, pmd_t **ppmd, spinlock_t **ptlp)
{
	pgd_t *pgd;
	pud_t *pud;
	pmd_t *pmd;

	pgd = pgd_offset(mm, addr);
	if (!pgd || pgd_none(*pgd)) return NULL;

	pud = pud_offset(pgd, addr);
	if (!pud || pud_none(*pud)) return NULL;

	pmd = pmd_offset(pud, addr);
	if (!pmd || pmd_none(*pmd)) return NULL;

	*ppmd = pmd;
	*ptlp = pte_lockptr(mm, pmd);

	return pte_offset_map(pmd, addr);
}

static pte_t *__get_pte_at_alloc(struct mm_struct *mm, struct vm_area_struct *vma, unsigned long addr, pmd_t **ppmd, spinlock_t **ptlp)
{
	pgd_t *pgd;
	pud_t *pud;
	pmd_t *pmd;
	pte_t *pte;

	pgd = pgd_offset(mm, addr);
	if (!pgd) return NULL;

	pud = pud_alloc(mm, pgd, addr);
	if (!pud) return NULL;

	pmd = pmd_alloc(mm, pud, addr);
	if (!pmd) return NULL;

	pte = pte_alloc_map(mm, vma, pmd, addr);

	*ppmd = pmd;
	*ptlp = pte_lockptr(mm, pmd);
	return pte;
}

static struct page *__find_page_at(struct mm_struct *mm, unsigned long addr, pte_t **ptep, spinlock_t **ptlp)
{
	pmd_t *pmd;
	pte_t *pte = NULL;
	spinlock_t *ptl = NULL;
	struct page *page = ERR_PTR(-ENOMEM);

	pte = __get_pte_at(mm, addr, &pmd, &ptl);

	if (pte == NULL) {
		pte = NULL;
		ptl = NULL;
		page = ERR_PTR(-EINVAL);
		goto out;
	}

	if (pte_none(*pte)) {
		pte_unmap(pte);
		pte = NULL;
		ptl = NULL;
		page = ERR_PTR(-ENOENT);
		goto out;
	}

	spin_lock(ptl);
	page = pte_page(*pte);
	get_page(page);

out:
	*ptep = pte;
	*ptlp = ptl;
	return page;
}


/**************************************************************************
 * Panicked by bug!!!!!
 */
void page_server_panic(bool condition, struct mm_struct *mm, unsigned long address, pte_t *pte, pte_t pte_val)
{
	unsigned long *pi;
	unsigned long pi_val = -1;
	unsigned long offset;
	if (!condition) return;

	pi = __get_page_info_mapped(mm, address, &offset);
	if (pi) {
		pi_val = *pi;
		kunmap_atomic(pi - offset);
	}

	printk(KERN_ERR "------------------ Start panicking -----------------\n");
	printk(KERN_ERR "%s: %lx %p %lx %p %lx\n", __func__,
			address, pi, pi_val, pte, pte_flags(pte_val));
	show_regs(current_pt_regs());
	BUG_ON("Page server panicked!!");
}


/**************************************************************************
 * Flush pages to the origin
 */
enum {
	FLUSH_FLAG_START = 0x01,
	FLUSH_FLAG_FLUSH = 0x02,
	FLUSH_FLAG_RELEASE = 0x04,
	FLUSH_FLAG_LAST = 0x10,
};


static void process_remote_page_flush(struct work_struct *work)
{
	START_KMSG_WORK(remote_page_flush_t, req, work);
	unsigned long addr = req->addr;
	struct task_struct *tsk;
	struct mm_struct *mm;
	struct remote_context *rc;
	struct page *page;
	pte_t *pte, entry;
	spinlock_t *ptl;
	void *paddr;
	struct vm_area_struct *vma;
	remote_page_flush_ack_t res = {
		.remote_ws = req->remote_ws,
	};

	PGPRINTK("  [%d] flush ->[%d/%d] %lx\n",
			req->origin_pid, req->remote_pid, req->remote_nid, addr);

	tsk = __get_task_struct(req->origin_pid);
	if (!tsk) goto out_free;

	mm = get_task_mm(tsk);
	rc = get_task_remote(tsk);

	if (req->flags & FLUSH_FLAG_START) {
		res.flags = FLUSH_FLAG_START;
		pcn_kmsg_send(PCN_KMSG_TYPE_REMOTE_PAGE_FLUSH_ACK,
				req->remote_nid, &res, sizeof(res));
		goto out_put;
	} else if (req->flags & FLUSH_FLAG_LAST) {
		res.flags = FLUSH_FLAG_LAST;
		pcn_kmsg_send(PCN_KMSG_TYPE_REMOTE_PAGE_FLUSH_ACK,
				req->remote_nid, &res, sizeof(res));
		goto out_put;
	}

	down_read(&mm->mmap_sem);
	vma = find_vma(mm, addr);
	BUG_ON(!vma || vma->vm_start > addr);

	page = __find_page_at(mm, addr, &pte, &ptl);
	BUG_ON(IS_ERR(page));

	/* XXX should be outside of ptl lock */
	if (req->flags & FLUSH_FLAG_FLUSH) {
		paddr = kmap(page);
		copy_to_user_page(vma, page, addr, paddr, req->page, PAGE_SIZE);
		kunmap(page);
	}

	SetPageDistributed(mm, addr);
	set_page_owner(my_nid, mm, addr);
	clear_page_owner(req->remote_nid, mm, addr);

	/* XXX Should update through clear_flush and set */
	entry = pte_make_valid(*pte);

	set_pte_at_notify(mm, addr, pte, entry);
	update_mmu_cache(vma, addr, pte);
	flush_tlb_page(vma, addr);

	put_page(page);

	pte_unmap_unlock(pte, ptl);
	up_read(&mm->mmap_sem);

out_put:
	put_task_remote(tsk);
	mmput(mm);
	put_task_struct(tsk);

out_free:
	END_KMSG_WORK(req);
}


static int __do_pte_flush(pte_t *pte, unsigned long addr, unsigned long next, struct mm_walk *walk)
{
	remote_page_flush_t *req = walk->private;
	struct vm_area_struct *vma = walk->vma;
	struct page *page;
	int req_size;
	enum pcn_kmsg_type req_type;
	char type;

	if (pte_none(*pte)) return 0;

	page = pte_page(*pte);
	BUG_ON(!page);

	if (test_page_owner(my_nid, vma->vm_mm, addr)) {
		req->addr = addr;
		if ((vma->vm_flags & VM_WRITE) && pte_write(*pte)) {
			void *paddr;
			flush_cache_page(vma, addr, page_to_pfn(page));
			paddr = kmap_atomic(page);
			copy_from_user_page(walk->vma, page, addr, req->page, paddr, PAGE_SIZE);
			kunmap_atomic(paddr);

			req_type = PCN_KMSG_TYPE_REMOTE_PAGE_FLUSH;
			req_size = sizeof(remote_page_flush_t);
			req->flags = FLUSH_FLAG_FLUSH;
			type = '*';
		} else {
			req_type = PCN_KMSG_TYPE_REMOTE_PAGE_RELEASE;
			req_size = sizeof(remote_page_release_t);
			req->flags = FLUSH_FLAG_RELEASE;
			type = '+';
		}
		clear_page_owner(my_nid, vma->vm_mm, addr);

		pcn_kmsg_send(req_type, current->origin_nid, req, req_size);
	} else {
		*pte = pte_make_valid(*pte);
		type = '-';
	}
	PGPRINTK("  [%d] %c %lx\n", current->pid, type, addr);

	return 0;
}


int page_server_flush_remote_pages(struct remote_context *rc)
{
	remote_page_flush_t *req = kmalloc(sizeof(*req), GFP_KERNEL);
	struct mm_struct *mm = rc->mm;
	struct mm_walk walk = {
		.pte_entry = __do_pte_flush,
		.mm = mm,
		.private = req,
	};
	struct vm_area_struct *vma;
	struct wait_station *ws = get_wait_station(current);

	BUG_ON(!req);

	PGPRINTK("FLUSH_REMOTE_PAGES [%d]\n", current->pid);

	req->remote_nid = my_nid;
	req->remote_pid = current->pid;
	req->remote_ws = ws->id;
	req->origin_pid = current->origin_pid;
	req->addr = 0;

	/* Notify the start synchronously */
	req->flags = FLUSH_FLAG_START;
	pcn_kmsg_send(PCN_KMSG_TYPE_REMOTE_PAGE_RELEASE,
			current->origin_nid, req, sizeof(*req));
	wait_at_station(ws);

	/* Send pages asynchronously */
	ws = get_wait_station(current);
	down_read(&mm->mmap_sem);
	for (vma = mm->mmap; vma; vma = vma->vm_next) {
		walk.vma = vma;
		walk_page_vma(vma, &walk);
	}
	up_read(&mm->mmap_sem);

	/* Notify the completion synchronously */
	req->flags = FLUSH_FLAG_LAST;
	pcn_kmsg_send(PCN_KMSG_TYPE_REMOTE_PAGE_FLUSH,
			current->origin_nid, req, sizeof(*req));
	wait_at_station(ws);

	kfree(req);

	// XXX: make sure there is no backlog.
	msleep(1000);

	return 0;
}

static int handle_remote_page_flush_ack(struct pcn_kmsg_message *msg)
{
	remote_page_flush_ack_t *req = (remote_page_flush_ack_t *)msg;
	struct wait_station *ws = wait_station(req->remote_ws);

	complete(&ws->pendings);

	pcn_kmsg_done(req);
	return 0;
}


/**************************************************************************
 * Page invalidation protocol
 */
static void __do_invalidate_page(struct task_struct *tsk, page_invalidate_request_t *req)
{
	struct mm_struct *mm = get_task_mm(tsk);
	struct vm_area_struct *vma;
	pmd_t *pmd;
	pte_t *pte, entry;
	spinlock_t *ptl;
	int ret = 0;
	unsigned long addr = req->addr;
	struct fault_handle *fh;

	down_read(&mm->mmap_sem);
	vma = find_vma(mm, addr);
	if (!vma || vma->vm_start > addr) {
		ret = VM_FAULT_SIGBUS;
		goto out;
	}

	PGPRINTK("\nINVALIDATE_PAGE [%d] %lx [%d/%d]\n", tsk->pid, addr,
			req->origin_pid, PCN_KMSG_FROM_NID(req));

	pte = __get_pte_at(mm, addr, &pmd, &ptl);
	if (!pte) goto out;

	spin_lock(ptl);
	fh = __start_invalidation(tsk, addr, ptl);

	clear_page_owner(my_nid, mm, addr);

	BUG_ON(!pte_present(*pte));
	entry = ptep_clear_flush(vma, addr, pte);
	entry = pte_make_invalid(entry);

	set_pte_at_notify(mm, addr, pte, entry);
	update_mmu_cache(vma, addr, pte);

	__finish_invalidation(fh);
	pte_unmap_unlock(pte, ptl);

out:
	up_read(&mm->mmap_sem);
	mmput(mm);
}

static void process_page_invalidate_request(struct work_struct *work)
{
	START_KMSG_WORK(page_invalidate_request_t, req, work);
	page_invalidate_response_t *res;
	struct task_struct *tsk;

	res = pcn_kmsg_get(sizeof(*res));
	res->origin_pid = req->origin_pid;
	res->origin_ws = req->origin_ws;
	res->remote_pid = req->remote_pid;

	/* Only home issues invalidate requests. Hence, I am a remote */
	tsk = __get_task_struct(req->remote_pid);
	if (!tsk) {
		PGPRINTK("%s: no such process %d %d %lx\n", __func__,
				req->origin_pid, req->remote_pid, req->addr);
		pcn_kmsg_put(res);
		goto out_free;
	}

	__do_invalidate_page(tsk, req);

	PGPRINTK(">>[%d] ->[%d/%d]\n", req->remote_pid, res->origin_pid,
			PCN_KMSG_FROM_NID(req));
	pcn_kmsg_post(PCN_KMSG_TYPE_PAGE_INVALIDATE_RESPONSE,
			PCN_KMSG_FROM_NID(req), res, sizeof(*res));

	put_task_struct(tsk);

out_free:
	END_KMSG_WORK(req);
}


static int handle_page_invalidate_response(struct pcn_kmsg_message *msg)
{
	page_invalidate_response_t *res = (page_invalidate_response_t *)msg;
	struct wait_station *ws = wait_station(res->origin_ws);

	if (atomic_dec_and_test(&ws->pendings_count)) {
		complete(&ws->pendings);
	}

	pcn_kmsg_done(res);
	return 0;
}


static void __revoke_page_ownership(struct task_struct *tsk, int nid, pid_t pid, unsigned long addr, int ws_id)
{
	page_invalidate_request_t *req = pcn_kmsg_get(sizeof(*req));

	req->addr = addr;
	req->origin_pid = tsk->pid;
	req->origin_ws = ws_id;
	req->remote_pid = pid;

	PGPRINTK("  [%d] revoke %lx [%d/%d]\n", tsk->pid, addr, pid, nid);
	pcn_kmsg_post(PCN_KMSG_TYPE_PAGE_INVALIDATE_REQUEST, nid, req, sizeof(*req));
}


/**************************************************************************
 * Voluntarily release page ownership
 * madvis() at remote (a page)
 *					->
 *							*here (origin)
 *					<-
 */
int process_madvise_release_from_remote(vma_op_request_t *req,
			int from_nid,
			unsigned long addr,
			unsigned long end)
{
	BUG();
#if 0
	int fail = VM_FAULT_RETRY;
	struct mm_struct *mm;
	int nr_pages = 0;
	struct task_struct *tsk;
	bool leader = false;
	struct fault_handle *fh;
	struct vm_area_struct *vma;
	pmd_t *pmd;
	pte_t *pte;
	spinlock_t *ptl;

	tsk = __get_task_struct(req->origin_pid);
	if (!tsk) goto out;

	mm = get_task_mm(tsk);
	if (!mm) {
		put_task_struct(tsk);
		goto out;
	}

	/* Origin didn't hold yet. Remote has hold at madvise() */
	down_read(&mm->mmap_sem);
	vma = find_vma(mm, addr);
    BUG_ON(!vma || vma->vm_start > addr);

	pte = __get_pte_at(mm, addr, &pmd, &ptl);
	if (!pte) goto put;

	spin_lock(ptl); // xxx: check: removing this should be fine.

	if (pte_none(*pte) || !pte_present(*pte)) {
		spin_unlock(ptl);
		goto put_fail;
	}

	fh = __start_releasing(tsk, addr, ptl, &leader);

	if (!leader) {
		goto put_fail;
	}

	/* check if the requester still own it */
	if (test_page_owner(from_nid, mm, addr)) {
		int nid;
		bool owners = false;
		unsigned long offset;
		unsigned long *pi;
		struct page *pip = __get_page_info_page(mm, addr, &offset);
		if (!pip) goto put_fail; /* skip claiming non-distributed page */
		pi = (unsigned long *)kmap(pip) + offset;
		/* clear only if there are more owners */
		for_each_set_bit(nid, pi, MAX_POPCORN_NODES) {
			if (from_nid == nid) continue;
			if (test_page_owner(nid, mm, addr)) {
				owners = true;
				break;
			}
		}
		if (owners) {
			spin_lock(ptl);
			clear_page_owner(from_nid, mm, addr); /* local table */
			spin_unlock(ptl);
			nr_pages++;
			fail = 0;
		}
	}

put_fail:
	if (leader) {
		fh->ret = fail;
		__finish_fault_handling(fh);
	}
	pte_unmap(pte);
put:
	up_read(&mm->mmap_sem);
	mmput(mm);
	put_task_struct(tsk);
out:
	PFPRINTK("(%s)handle RELEASE req at ori:  [%d] %d %d / %ld %lx-%lx\n",
			fail?"X":"O",
			req->remote_pid , from_nid,
			nr_pages, (end - addr) / PAGE_SIZE, addr, end);
	return fail;
#endif
}

/* page must be mine since origin doone
 *
 * at_origin
 * lock
 * remote (success done) -> at_remote (only revoke request node)
 * origin (revoke my local)
 * unlock
 * local update
 * claim_local_page
 * __revoke_page_ownership PCN_KMSG_TYPE_PAGE_INVALIDATE_REQUEST
 * 		process_page_invalidate_request (must done)
 *			__do_invalidate_page							!!!!!!!!!!!!!!!
 * 			<-
 * handle_page_invalidate_response
 * complete
 */
int release_page_ownership_at_local(struct vm_area_struct *vma, unsigned long addr)
{
	struct mm_struct *mm = vma->vm_mm;
	pmd_t *pmd;
	pte_t *pte;
	pte_t pte_val;
	spinlock_t *ptl;

	pte = __get_pte_at(mm, addr, &pmd, &ptl);
	BUG_ON(!pte); 	/* if so, handle it */

	spin_lock(ptl);

#ifdef CONFIG_POPCORN_CHECK_SANITY
	BUG_ON(pte_none(*pte));			/* no pg frame (can ignore but how about performance?) */
	BUG_ON(!pte_present(*pte));		/* must succeed */
#endif

	clear_page_owner(my_nid, mm, addr);
	pte_val = ptep_clear_flush(vma, addr, pte);
	pte_val = pte_make_invalid(pte_val);

	set_pte_at_notify(mm, addr, pte, pte_val);
	update_mmu_cache(vma, addr, pte);

	pte_unmap_unlock(pte, ptl);
	return 1;
}

/*
 * This function is still under working NOT YET DONE!!!!!!!!!!!!!!!!!!!!
 * Hanlde releasing a singl addr request form madvise
 * return err: 1/0
 */
int vma_server_madvise_remote(unsigned long start, size_t len, int behavior);
int page_server_release_page_ownership(struct vm_area_struct *vma,
												unsigned long addr)
{
	BUG();
#if 0
	struct mm_struct *mm = vma->vm_mm;
	pmd_t *pmd;
	pte_t *pte;
	spinlock_t *ptl;
	struct fault_handle *fh;
	bool leader = false;
	int ret = VM_FAULT_RETRY;
	int err = -1;
	if (!page_is_mine(mm, addr)) return -1; /* origin/remote check as well */

	pte = __get_pte_at(mm, addr, &pmd, &ptl);
	if (!pte) return -1; /* possible */

	spin_lock(ptl); /* check: try to not hold this pte */

	/* !present means I'm not the owner aka has been invalidated */
	if (pte_none(*pte) || !pte_present(*pte)) {
		pte_unmap_unlock(pte, ptl);
		return -1;
	}

	fh = __start_releasing(current, addr, ptl, &leader);

	if (!leader)
		goto fail;

	pf_action_release_record(true, false);
	if (current->at_remote) {
		/* 1. Remote */
		err = vma_server_madvise_remote(addr, PAGE_SIZE, MADV_RELEASE);
		/* -> process_madvise_release_from_remote
		 * __start_fault_handling
		 * problem with prefetch_at_origin O
		 * vma_server_madvise_remote()
		 * at remote             			at origin
		 * remote (check if only onwer)
		 * __delegate_vma_op
		 * 			PCN_KMSG_TYPE_VMA_OP_REQUEST
		 * res = wait_at_station(ws);
		 *           		-> process_vma_op_request
		 *              		 	__process_vma_op_at_remote (not implemented)
		 *               		__process_vma_op_at_origin
		 *                   		VMA_OP_MADVISE
		 *                   		[ret] = process_madvise_release_from_remote
		 *           		<-__reply_vma_op
		 * res
		 * [return ret]
		 */
		if (err) {
			pf_action_release_record(false, false);
			goto fail;
		}
		/* 2. Local Origin - lock*/ // prolem with select_prefetch_pages()
		release_page_ownership_at_local(vma, addr); /* local */ /*page must be mine since origin done*/
		ret = 0;
		pf_action_release_record(false, true);
	} else {
		/* Working on */
		//BUG();
		err = -1;
		// 1. local Origin
		//nr_pages += release_page_ownership_at_local(vma, addr);
		//release_page_ownership_at_local(vma, addr);

		// 2. remote must done
		//
	}

fail:
	if (leader) {
		fh->ret = ret;
		//fh->ret = VM_FAULT_RETRY;
		__finish_fault_handling(fh);
	}
	pte_unmap(pte);
	/*
	if (err > 0)
		PFPRINTK("[%d]o RELEASE at origin NOT IMPLEMENTED YET\n", current->pid);
	else
		PFPRINTK("[%d]r RELEASE (%s)\n", current->pid, err? "X" : "O");
	*/
	return err;
#endif
}

/**************************************************************************
 * Handle page faults happened at remote nodes.
 */
static int handle_remote_page_response(struct pcn_kmsg_message *msg)
{
	remote_page_response_t *res = (remote_page_response_t *)msg;
	struct wait_station *ws = wait_station(res->origin_ws);

	PGPRINTK("  [%d] <-[%d/%d] %lx %x\n",
			ws->pid, res->remote_pid, PCN_KMSG_FROM_NID(res),
			res->addr, res->result);
	ws->private = res;

	if (atomic_dec_and_test(&ws->pendings_count))
		complete(&ws->pendings);
	return 0;
}

#define TRANSFER_PAGE_WITH_RDMA \
		pcn_kmsg_has_features(PCN_KMSG_FEATURE_RDMA)

static int __request_remote_page(struct task_struct *tsk, int from_nid, pid_t from_pid, unsigned long addr, unsigned long fault_flags, int ws_id, struct pcn_kmsg_rdma_handle **rh, struct prefetch_list* pf_list, int pf_nr_pages)
{
	remote_page_request_t *req;
	int size;

	*rh = NULL;

	req = pcn_kmsg_get(sizeof(*req));
	req->addr = addr;
	req->fault_flags = fault_flags;

	req->origin_pid = tsk->pid;
	req->origin_ws = ws_id;

	req->remote_pid = from_pid;
	req->instr_addr = instruction_pointer(current_pt_regs());

	if (pf_list && pf_nr_pages > 0) {
		req->pf_list_size = pf_nr_pages;
		memcpy(&req->pf_list, pf_list, sizeof(struct prefetch_list));
		size = sizeof(*req);
	} else {
		req->pf_list_size = 0;
		size = sizeof(*req) - sizeof(struct prefetch_list);
	}

	if (TRANSFER_PAGE_WITH_RDMA) {
		struct pcn_kmsg_rdma_handle *handle =
				pcn_kmsg_pin_rdma_buffer(NULL, PAGE_SIZE);
		if (IS_ERR(handle)) {
			pcn_kmsg_put(req);
			return PTR_ERR(handle);
		}
		*rh = handle;
		req->rdma_addr = handle->dma_addr;
		req->rdma_key = handle->rkey;
	} else {
		req->rdma_addr = 0;
		req->rdma_key = 0;
	}

	PGPRINTK("  [%d] ->[%d/%d] %lx %lx\n", tsk->pid,
			from_pid, from_nid, addr, req->instr_addr);

	pcn_kmsg_post(PCN_KMSG_TYPE_REMOTE_PAGE_REQUEST,
						//from_nid, req, sizeof(*req));
						from_nid, req, size);
	return 0;
}

static remote_page_response_t *__fetch_page_from_origin(struct task_struct *tsk, struct vm_area_struct *vma, unsigned long addr, unsigned long fault_flags, struct page *page, struct prefetch_list* pf_list, int pf_nr_pages)
{
	remote_page_response_t *rp;
	struct wait_station *ws = get_wait_station(tsk);
	struct pcn_kmsg_rdma_handle *rh;

	__request_remote_page(tsk, tsk->origin_nid, tsk->origin_pid,
						addr, fault_flags, ws->id, &rh, pf_list, pf_nr_pages);

	rp = wait_at_station(ws);
	if (rp->result == 0) {
		void *paddr = kmap(page);
		if (TRANSFER_PAGE_WITH_RDMA) {
			copy_to_user_page(vma, page, addr, paddr, rh->addr, PAGE_SIZE);
		} else {
			copy_to_user_page(vma, page, addr, paddr, rp->page, PAGE_SIZE);
		}
		kunmap(page);
		flush_dcache_page(page);
		__SetPageUptodate(page);
	}

	if (rh) pcn_kmsg_unpin_rdma_buffer(rh);

	return rp;
}

static int __claim_remote_page(struct task_struct *tsk, struct mm_struct *mm, struct vm_area_struct *vma, unsigned long addr, unsigned long fault_flags, struct page *page)
{
	int peers;
	unsigned int random = prandom_u32();
	struct wait_station *ws;
	struct remote_context *rc = __get_mm_remote(mm);
	remote_page_response_t *rp;
	int from, from_nid;
	/* Read when @from becomes zero and save the nid to @from_nid */
	int nid, result;
	struct pcn_kmsg_rdma_handle *rh = NULL;
	unsigned long offset;
	struct page *pip = __get_page_info_page(mm, addr, &offset);
	unsigned long *pi = (unsigned long *)kmap(pip) + offset;
	BUG_ON(!pip);

	peers = bitmap_weight(pi, MAX_POPCORN_NODES);

	if (test_bit(my_nid, pi)) {
		peers--;
	}
#ifdef CONFIG_POPCORN_CHECK_SANITY
	page_server_panic(peers == 0, mm, addr, NULL, __pte(0));
#endif
	from = random % peers;

	// PGPRINTK("  [%d] fetch %lx from %d peers\n", tsk->pid, addr, peers);

	if (fault_for_read(fault_flags)) {
		peers = 1;
	}
	ws = get_wait_station_multiple(tsk, peers);

	for_each_set_bit(nid, pi, MAX_POPCORN_NODES) {
		pid_t pid = rc->remote_tgids[nid];
		if (nid == my_nid) continue;
		if (from-- == 0) {
			from_nid = nid;
			__request_remote_page(tsk, nid, pid, addr, fault_flags, ws->id, &rh, NULL, 0);
		} else {
			if (fault_for_write(fault_flags)) {
				clear_bit(nid, pi);
				__revoke_page_ownership(tsk, nid, pid, addr, ws->id);
			}
		}
		if (--peers == 0) break;
	}

	rp = wait_at_station(ws);

	if (fault_for_write(fault_flags)) {
		clear_bit(from_nid, pi);
	}

	if (rp->result == 0) {
		void *paddr = kmap(page);
		if (TRANSFER_PAGE_WITH_RDMA) {
			copy_to_user_page(vma, page, addr, paddr, rh->addr, PAGE_SIZE);
		} else {
			copy_to_user_page(vma, page, addr, paddr, rp->page, PAGE_SIZE);
		}
		kunmap(page);
		flush_dcache_page(page);
		__SetPageUptodate(page);
	}
	result = rp->result;
	pcn_kmsg_done(rp);

	if (rh) pcn_kmsg_unpin_rdma_buffer(rh);
	__put_task_remote(rc);
	kunmap(pip);
	return result;
}

static void __claim_local_page(struct task_struct *tsk, unsigned long addr, int except_nid, struct mm_struct *mm)
{
	unsigned long offset;
	struct page *pip = __get_page_info_page(mm, addr, &offset);
	unsigned long *pi;
	int peers;

	if (!pip) return; /* skip claiming non-distributed page */
	pi = (unsigned long *)kmap(pip) + offset;
	peers = bitmap_weight(pi, MAX_POPCORN_NODES);
	if (!peers) {
		kunmap(pip);
		return;	/* skip claiming the page that is not distributed */
	}

	BUG_ON(!test_bit(except_nid, pi));
	peers--;	/* exclude except_nid from peers */

	if (test_bit(my_nid, pi) && except_nid != my_nid) peers--;

	if (peers > 0) {
		int nid;
		struct remote_context *rc = __get_mm_remote(mm);
		struct wait_station *ws = get_wait_station_multiple(tsk, peers);

		for_each_set_bit(nid, pi, MAX_POPCORN_NODES) {
			pid_t pid = rc->remote_tgids[nid];
			if (nid == except_nid || nid == my_nid) continue;

			clear_bit(nid, pi);
			__revoke_page_ownership(tsk, nid, pid, addr, ws->id);
		}
		__put_task_remote(rc);

		wait_at_station(ws);
	}
	kunmap(pip);
}

void page_server_zap_pte(struct vm_area_struct *vma, unsigned long addr, pte_t *pte, pte_t *pteval)
{
	if (!vma->vm_mm->remote) return;

	ClearPageInfo(vma->vm_mm, addr);

	*pteval = pte_make_valid(*pte);
	*pteval = pte_mkyoung(*pteval);
	if (ptep_set_access_flags(vma, addr, pte, *pteval, 1)) {
		update_mmu_cache(vma, addr, pte);
	}
#ifdef CONFIG_POPCORN_DEBUG_VERBOSE
	PGPRINTK("  [%d] zap %lx\n", current->pid, addr);
#endif
}

static void __make_pte_valid(struct mm_struct *mm,
		struct vm_area_struct *vma, unsigned long addr,
		unsigned long fault_flags, pte_t *pte)
{
	pte_t entry;

	entry = ptep_clear_flush(vma, addr, pte);
	entry = pte_make_valid(entry);

	if (fault_for_write(fault_flags)) {
		entry = pte_mkwrite(entry);
		entry = pte_mkdirty(entry);
	} else {
		entry = pte_wrprotect(entry);
	}
	entry = pte_mkyoung(entry);

	set_pte_at_notify(mm, addr, pte, entry);
	update_mmu_cache(vma, addr, pte);
	// flush_tlb_page(vma, addr);

	SetPageDistributed(mm, addr);
	set_page_owner(my_nid, mm, addr);
}


/**************************************************************************
 * Remote fault handler at a remote location
 */
static int __handle_remotefault_at_remote(struct task_struct *tsk, struct mm_struct *mm, struct vm_area_struct *vma, remote_page_request_t *req, remote_page_response_t *res)
{
	unsigned long addr = req->addr;
	unsigned fault_flags = req->fault_flags | FAULT_FLAG_REMOTE;
	unsigned char *paddr;
	struct page *page;

	spinlock_t *ptl;
	pmd_t *pmd;
	pte_t *pte;
	pte_t entry;

	struct fault_handle *fh;
	bool leader;

	pte = __get_pte_at(mm, addr, &pmd, &ptl);
	if (!pte) {
		PGPRINTK("  [%d] No PTE!!\n", tsk->pid);
		return VM_FAULT_OOM;
	}

	spin_lock(ptl);
	fh = __start_fault_handling(tsk, addr, fault_flags, ptl, &leader);
	if (!fh) {
		pte_unmap(pte);
		return VM_FAULT_LOCKED;
	}

	if (pte_none(*pte)) {
		pte_unmap(pte);
		return VM_FAULT_SIGSEGV;
	}

#ifdef CONFIG_POPCORN_CHECK_SANITY
	BUG_ON(!page_is_mine(mm, addr));
#endif

	spin_lock(ptl);
	SetPageDistributed(mm, addr);
	entry = ptep_clear_flush(vma, addr, pte);

	if (fault_for_write(fault_flags)) {
		clear_page_owner(my_nid, mm, addr);
		entry = pte_make_invalid(entry);
	} else {
		entry = pte_wrprotect(entry);
	}

	set_pte_at_notify(mm, addr, pte, entry);
	update_mmu_cache(vma, addr, pte);
	pte_unmap_unlock(pte, ptl);

	page = vm_normal_page(vma, addr, *pte);
	BUG_ON(!page);
	flush_cache_page(vma, addr, page_to_pfn(page));
	if (TRANSFER_PAGE_WITH_RDMA) {
		paddr = kmap(page);
		pcn_kmsg_rdma_write(PCN_KMSG_FROM_NID(req),
				req->rdma_addr, paddr, PAGE_SIZE, req->rdma_key);
		kunmap(page);
	} else {
		paddr = kmap_atomic(page);
		copy_from_user_page(vma, page, addr, res->page, paddr, PAGE_SIZE);
		kunmap_atomic(paddr);
	}

	__finish_fault_handling(fh);
	return 0;
}



/**************************************************************************
 * Remote fault handler at the origin
 */
static int __handle_remotefault_at_origin(struct task_struct *tsk, struct mm_struct *mm, struct vm_area_struct *vma, remote_page_request_t *req, remote_page_response_t *res)
{
	int from_nid = PCN_KMSG_FROM_NID(req);
	unsigned long addr = req->addr;
	unsigned long fault_flags = req->fault_flags | FAULT_FLAG_REMOTE;
	unsigned char *paddr;
	struct page *page;

	spinlock_t *ptl;
	pmd_t *pmd;
	pte_t *pte;

	struct fault_handle *fh;
	bool leader;
	bool grant = false;

again:
	pte = __get_pte_at_alloc(mm, vma, addr, &pmd, &ptl);
	if (!pte) {
		PGPRINTK("  [%d] No PTE!!\n", tsk->pid);
		return VM_FAULT_OOM;
	}

	spin_lock(ptl);
	if (pte_none(*pte)) {
		int ret;
		spin_unlock(ptl);
		PGPRINTK("  [%d] handle local fault at origin\n", tsk->pid);
		ret = handle_pte_fault_origin(mm, vma, addr, pte, pmd, fault_flags);
		/* returned with pte unmapped */
		if (ret & VM_FAULT_RETRY) {
			/* mmap_sem is released during do_fault */
			return VM_FAULT_RETRY;
		}
		if (fault_for_write(fault_flags) && !vma_is_anonymous(vma))
			SetPageCowed(mm, addr);
		goto again;
	}

	fh = __start_fault_handling(tsk, addr, fault_flags, ptl, &leader);

	/**
	 * Indicates the same page is handled at the origin and it might cause
	 * this node to be blocked recursively. This prevents forming the loop
	 * by releasing everything from remote.
	 */
	if (!fh) {
		pte_unmap(pte);
		up_read(&mm->mmap_sem); /* To match the sematic for VM_FAULT_RETRY */
		return VM_FAULT_RETRY;
	}

	page = get_normal_page(vma, addr, pte); /* no matter what get a pg */

	if (leader) {
		pte_t entry;

		/* Prepare the page if it is not mine. This should be leader */
		PGPRINTK(" =[%d] %s%s %p\n",
				tsk->pid, page_is_mine(mm, addr) ? "origin " : "",
				test_page_owner(from_nid, mm, addr) ? "remote": "", fh);

		if (test_page_owner(from_nid, mm, addr)) {
			BUG_ON(fault_for_read(fault_flags) && "Read fault from owner??");
			__claim_local_page(tsk, addr, from_nid, mm);
			grant = true;
		} else {
			if (!page_is_mine(mm, addr)) {
				__claim_remote_page(tsk, mm, vma, addr, fault_flags, page);
			} else {
				if (fault_for_write(fault_flags))
					__claim_local_page(tsk, addr, my_nid, mm);
			}
		}

		spin_lock(ptl);

		SetPageDistributed(mm, addr);
		set_page_owner(from_nid, mm, addr);

		entry = ptep_clear_flush(vma, addr, pte);
		if (fault_for_write(fault_flags)) {
			clear_page_owner(my_nid, mm, addr);
			entry = pte_make_invalid(entry);
		} else {
			entry = pte_make_valid(entry); /* For remote-claimed case */
			entry = pte_wrprotect(entry);
			set_page_owner(my_nid, mm, addr);
		}
		set_pte_at_notify(mm, addr, pte, entry);
		update_mmu_cache(vma, addr, pte);

		spin_unlock(ptl);
	}
	pte_unmap(pte);

	if (!grant) {
		flush_cache_page(vma, addr, page_to_pfn(page));
		if (TRANSFER_PAGE_WITH_RDMA) {
			paddr = kmap(page);
			pcn_kmsg_rdma_write(PCN_KMSG_FROM_NID(req),
					req->rdma_addr, paddr, PAGE_SIZE, req->rdma_key);
			kunmap(page);
		} else {
			paddr = kmap_atomic(page);
			copy_from_user_page(vma, page, addr, res->page, paddr, PAGE_SIZE);
			kunmap_atomic(paddr);
		}
	}

	__finish_fault_handling(fh);
	return grant ? VM_FAULT_CONTINUE : 0;
}

/*
 * Pre-prefetch from a remote remote node
 * This supports only for PF READ, since PF WRITE cannot use pre-prefetch.
 * It has to be done immediately.
 */
static void __do_preprefetch(struct list_head *preprefetch_list_head,
							int from_nid, struct task_struct *tsk,
							struct mm_struct *mm, struct remote_context *rc)
{
#if PREFETCH_REMOTE_REMOTE_SUPPORT
#if !PREFETCH_REMOTE_REMOTE_IMM
	struct got_remote_page_prefetch_req *curr, *next;
	if (list_empty(preprefetch_list_head)) goto out;

	list_for_each_entry_safe(curr, next, preprefetch_list_head, list) {
		unsigned long addr = curr->addr;
		unsigned long fault_flags = curr->flags;
		unsigned long flags;
		struct fault_handle *fh = NULL;
		bool found = false, leader =false;
		int fk;
		pmd_t *pmd;
		pte_t *pte;
		spinlock_t *ptl;
		struct vm_area_struct *vma = find_vma(mm, addr); //TODO
		if (!vma || vma->vm_start > addr) BUG();

		pte = __get_pte_at(mm, addr, &pmd, &ptl);
		if (!pte) BUG();

		spin_lock(ptl);
		fk = __fault_hash_key(addr);
		spin_lock_irqsave(&rc->faults_lock[fk], flags);
		spin_unlock(ptl);

		hlist_for_each_entry(fh, &rc->faults[fk], list) {
			if (fh->addr == addr) {
				found = true;
				break;
			}
		}

		if (!found) {
			fh = __alloc_fault_handle(tsk, addr, rc);
			fh->flags = fault_flags; // check ???
			leader = true;
		} /* if found just pass */
		spin_unlock_irqrestore(&rc->faults_lock[fk], flags);

		if (leader) {
			/* double check since page status may have been changed */
			if (!test_page_owner(from_nid, mm, addr)) {
				if (!page_is_mine(mm, addr)) {
					int err;
					struct page *page = vm_normal_page(vma, addr, *pte);
#ifdef CONFIG_POPCORN_CHECK_SANITY
					BUG_ON(!page);
#endif
					err = __claim_remote_page(tsk, mm, vma, addr, fault_flags, page); //xx
#ifdef CONFIG_POPCORN_CHECK_SANITY
					BUG_ON(err); // TODO: why 100% success? ask?
#endif
					spin_lock(ptl);
					__make_pte_valid(mm, vma, addr, fault_flags, pte);
					spin_unlock(ptl);
				} else {
					/* Origin has had the page. Perfetch for pre-prefetch */
					pfpf_fail_action_record(true, false, false);
				}
			} else { /* requesting node has had the page */
				/* with pf_read, this is done */
				/* with pf_write, this is a read->write req. TODO*/
				pfpf_fail_action_record(false, true, false);
			}
			__finish_fault_handling(fh);
		} else {
			pfpf_fail_action_record(false, false, true);
		}

		pte_unmap(pte);

		list_del(&curr->list);
		kfree(curr);
	}
out:
#endif
#endif
	return;
}

inline static int __prepare_get_remote_page(struct fault_handle **fh,
							struct task_struct *tsk, unsigned long addr,
							struct remote_context *rc, bool *leader,
							bool *prepare_to_get_remote_page,
							struct prefetch_body *list_ptr)
{
#if !PREFETCH_REMOTE_REMOTE_SUPPORT
	/* prefetch remote remote - not support */
	pf_action_fail_record_origin_pg_not_mine();
	return -1;
#else
#if PREFETCH_REMOTE_REMOTE_IMM
	/* prefetch remote remote - support */
	*leader = true;
	*fh = __alloc_fault_handle(tsk, addr, rc);

	/* remotefault | at remote | read/wirte */
	((struct fault_handle*)*fh)->flags = FAULT_HANDLE_REMOTE;
	if (list_ptr->is_write)
		((struct fault_handle*)*fh)->flags |= FAULT_FLAG_WRITE;
	return 0;
#else
	*prepare_to_get_remote_page = true;
	return -1;
#endif
#endif
}

inline static int __get_remote_page(struct mm_struct *mm, struct page **page,
						struct vm_area_struct *vma, unsigned long addr, pte_t *pte,
						struct task_struct *tsk, struct fault_handle *fh,
													bool *got_remote_page)
{
#if PREFETCH_REMOTE_REMOTE_SUPPORT
#if PREFETCH_REMOTE_REMOTE_IMM
	*page = get_normal_page(vma, addr, pte); /* get a pg on-demandly */
	*got_remote_page = true;
	__claim_remote_page(tsk, mm, vma, addr, fh->flags, *page);
	return 0;
#endif
#endif
	return -1;
}

inline static int __pre_prefetch_req_add(bool prepare_to_get_remote_page,
										struct list_head *preprefetch_list_head,
										unsigned long addr,
										struct prefetch_body *list_ptr)
{
#if PREFETCH_REMOTE_REMOTE_SUPPORT
#if !PREFETCH_REMOTE_REMOTE_IMM
	if (prepare_to_get_remote_page) {
		struct got_remote_page_prefetch_req *rrpf_req =
						kmalloc(sizeof(*rrpf_req), GFP_KERNEL);

		rrpf_req->addr = addr;
		rrpf_req->flags = FAULT_HANDLE_REMOTE;
		if (list_ptr->is_write)
			rrpf_req->flags |= FAULT_FLAG_WRITE;

		/* This is sequential. No lock needed. */
		list_add_tail(&rrpf_req->list, preprefetch_list_head);
		return -1;
	}
#endif
#endif
	return 0;
}

static void __prefetch_each_at_origin(struct mm_struct *mm, struct vm_area_struct *vma,
					struct task_struct *tsk, struct remote_context *rc,
					struct prefetch_body *list_curr, int pf_num,
					int *res_size, int succ_pf_num,
					int from_nid, remote_prefetch_response_t *res,
					struct list_head *preprefetch_list_head)
{
	int fk;
	pmd_t *pmd;
	pte_t *pte, entry;
	void *paddr;
	spinlock_t *ptl;
	struct page *page;
	unsigned long flags;
	bool got_remote_page = false;
	struct fault_handle *fh = NULL;
	bool is_write = list_curr->is_write;
	unsigned long addr = list_curr->addr;
	bool found = false, leader = false, grant = false;
	bool prepare_to_get_remote_page = false;

	res->addr[pf_num] = addr;
	res->is_write[pf_num] = is_write;
	res->result[pf_num] = PREFETCH_FAIL;

	pte = __get_pte_at(mm, addr, &pmd, &ptl);

	// xxx: test: removing ptl directly
//	if (is_besteffort) {
		spin_lock(ptl);
//	} else {
//		if (!spin_trylock(ptl)) {
//			PFPRINTK("origin unselect %lx pte locked\n", addr);
///			res->result[pf_num] = PREFETCH_FAIL;
//			goto next;
//		}
//	}

	fk = __fault_hash_key(addr);

//	if (is_besteffort) {
		spin_lock_irqsave(&rc->faults_lock[fk], flags);
//	} else {
//		if (!spin_trylock_irqsave(&rc->faults_lock[fk], flags)) {
//			spin_unlock(ptl);
//			PFPRINTK("origin unselect %lx fh locked\n", addr);
//			res->result[pf_num] = PREFETCH_FAIL;
//			goto next;
//		}
//	}
	spin_unlock(ptl);

	hlist_for_each_entry(fh, &rc->faults[fk], list) {
		if (fh->addr == addr) {
			found = true;
			break;
		}
	}

	if (found) {
		res->result[pf_num] = PREFETCH_FAIL;
		pf_action_fail_record_origin_busy();

	} else {
		if (page_is_mine(mm, addr)) {
			leader = true;
			res->result[pf_num] = PREFETCH_SUCCESS;
			*res_size += PAGE_SIZE;
			fh = __alloc_fault_handle(tsk, addr, rc);

			/* remotefault | at remote | read/wirte */
			fh->flags = FAULT_HANDLE_REMOTE;
			if (is_write)
				fh->flags |= FAULT_HANDLE_WRITE;
		} else {
			if (!__prepare_get_remote_page(&fh, tsk, addr, rc, &leader,
						&prepare_to_get_remote_page, list_curr)) {
				*res_size += PAGE_SIZE;
			} else {
				res->result[pf_num] = PREFETCH_FAIL;
			}
		}
	}
	spin_unlock_irqrestore(&rc->faults_lock[fk], flags);

	if (!leader)
		goto next;

	if (test_page_owner(from_nid, mm, addr)) { /* R->W */
		BUG_ON(!is_write && "Read fault req from a owner??");
		__claim_local_page(tsk, addr, from_nid, mm);
		grant = true; /* the remote  has had the up-to-date pg */
		res->result[pf_num] |= VM_FAULT_CONTINUE;
	} else {
		if (!page_is_mine(mm, addr)) { /* remote-remote: X->R/W */
			if (!__get_remote_page(mm, &page, vma, addr,
									pte, tsk, fh, &got_remote_page))
				res->result[pf_num] = PREFETCH_SUCCESS;
		} else { /* owner = me */
			if (is_write) { /* X->W revoke others */
				__claim_local_page(tsk, addr, from_nid, mm);
			} else { /* X->R don't revoke */
				;
			}
		}
	}

	spin_lock(ptl);
	SetPageDistributed(mm, addr);
	set_page_owner(from_nid, mm, addr);	/* No matter pf with read/write permission!! */
	entry = ptep_clear_flush(vma, addr, pte);

	if (is_write) { /* remotefault | write */
		clear_page_owner(my_nid, mm, addr); /* I cannot be the owner, req node will be */
		entry = pte_make_invalid(entry);
	} else { /* remotefault | read */
		entry = pte_make_valid(entry); /* For remote-claimed case */
		entry = pte_wrprotect(entry);
		set_page_owner(my_nid, mm, addr); /* Shared owners */
	}
	set_pte_at_notify(mm, addr, pte, entry);
	update_mmu_cache(vma, addr, pte);
	spin_unlock(ptl);

	/* copy page to msg */
	if (!grant) {
		if (!got_remote_page) {
			page = get_normal_page(vma, addr, pte); /* get a pg on-demandly */
		}
		flush_cache_page(vma, addr, page_to_pfn(page));

//		if (TRANSFER_PAGE_WITH_RDMA) { //ooo
//			paddr = kmap(page);
//			pcn_kmsg_rdma_write(from_nid),
//			list_curr->rdma_addr, paddr, PAGE_SIZE, list_curr->rdma_key);
//			kunmap(page);
//		} else {
			paddr = kmap_atomic(page);
			copy_from_user_page(vma, page, addr,
				&res->page[succ_pf_num][0], paddr, PAGE_SIZE);
			kunmap_atomic(paddr);
//		}
	}
	__finish_fault_handling(fh);

	if (__pre_prefetch_req_add(prepare_to_get_remote_page,
						preprefetch_list_head, addr, list_curr))
		res->result[pf_num] = PREFETCH_FAIL;

next:
	pte_unmap(pte);

	PFPRINTK("handled pf r%d-#%d:\t%lx (%s)\n",
			from_nid, pf_num, addr,
			res->result[pf_num] & PREFETCH_SUCCESS ? "O" : "X");
}


/* process prefetch requests at origin */
int __prefetch_at_origin(int from_nid, int remote_pid, int origin_pid,
						int pf_list_size, struct prefetch_body *list_curr)
{
	int cnt = 0, res_size = 0, succ_pf_num = 0;
	struct mm_struct *mm;
	struct task_struct *tsk;
	struct timeval tv_start;
    struct remote_context *rc;
	struct vm_area_struct *vma;
	remote_prefetch_response_t *res;
	struct list_head preprefetch_list_head;
	if (!pf_list_size) return -1;
#ifdef CONFIG_POPCORN_CHECK_SANITY
	BUG_ON(!list_curr->addr && "is_list but no content");
#endif
	pf_time_start_at_origin(&tv_start);

//	if (TRANSFER_PAGE_WITH_RDMA) { //ooo
//		res = pcn_kmsg_get(sizeof(remote_prefetch_response_short_t));
//	} else {
		res = pcn_kmsg_get(sizeof(*res));
//	}
	
	tsk = __get_task_struct(remote_pid);
	if (!tsk) BUG();

	mm = get_task_mm(tsk);
	if (!mm) {
		put_task_struct(tsk);
		BUG();
	}

	rc = __get_mm_remote(mm);

	down_read(&mm->mmap_sem);
	vma = find_vma(mm, list_curr->addr);
	if (!vma || vma->vm_start > list_curr->addr) {
		PFPRINTK("origin %lx is_vma(%s) or wrong addr range\n",
								list_curr->addr, vma ? "O" : "X");
		BUG();
	}

	res_size = sizeof(remote_prefetch_response_short_t); /* short msg size + pages */
	while (list_curr->addr && cnt < pf_list_size) {
		__prefetch_each_at_origin(mm, vma, tsk, rc, list_curr, cnt,
									&res_size, succ_pf_num, from_nid,
									res, &preprefetch_list_head);
		if (res->result[cnt] & PREFETCH_SUCCESS)
			succ_pf_num++;

		cnt++;
		list_curr++;
    }
	__do_preprefetch(&preprefetch_list_head, from_nid, tsk, mm, rc);

	PFPRINTK("pf response -> [%d] %d/%d %d", origin_pid,
					succ_pf_num,  pf_list_size, res_size);
	res->remote_pid = remote_pid;
	res->origin_pid = origin_pid;
	pcn_kmsg_post(PCN_KMSG_TYPE_REMOTE_PREFETCH_RESPONSE,
								from_nid, res, res_size);

	up_read(&mm->mmap_sem);
	__put_task_remote(rc);
	mmput(mm);
	put_task_struct(tsk);
	pf_time_end_at_origin(&tv_start);
	return 0;
}

static void process_remote_prefetch_request(struct work_struct *work)
{
	START_KMSG_WORK(remote_prefetch_request_t, req, work);
	struct task_struct *tsk = __get_task_struct(req->remote_pid);
	int from_nid = PCN_KMSG_FROM_NID(req);

	/* Only support remote prefetch */
	if (!tsk->at_remote)
		__prefetch_at_origin(from_nid, req->remote_pid, req->origin_pid,
					req->pf_list_size, (struct prefetch_body*)&req->pf_list);

	if (tsk) put_task_struct(tsk);
	END_KMSG_WORK(req);
}

/**
 * Entry point to remote fault handler
 *
 * To accelerate the ownership grant by skipping transferring page data,
 * the response might be multiplexed between remote_page_response_short_t and
 * remote_page_response_t.
 */
static void process_remote_page_request(struct work_struct *work)
{
	START_KMSG_WORK(remote_page_request_t, req, work);
	int from_nid = PCN_KMSG_FROM_NID(req);
	int res_size, down_read_retry = 0;
	remote_page_response_t *res;
	enum pcn_kmsg_type res_type;
	struct vm_area_struct *vma;
	struct task_struct *tsk;
	struct mm_struct *mm;
	bool at_remote;

	if (TRANSFER_PAGE_WITH_RDMA) {
		res = pcn_kmsg_get(sizeof(remote_page_response_short_t));
	} else {
		res = pcn_kmsg_get(sizeof(*res));
	}

again:
	tsk = __get_task_struct(req->remote_pid);
	if (!tsk) {
		res->result = VM_FAULT_SIGBUS;
		PGPRINTK("  [%d] not found\n", req->remote_pid);
		goto out;
	}
	mm = get_task_mm(tsk);

	PGPRINTK("\nREMOTE_PAGE_REQUEST [%d] %lx %c %lx from [%d/%d]\n",
			req->remote_pid, req->addr,
			fault_for_write(req->fault_flags) ? 'W' : 'R',
			req->instr_addr, req->origin_pid, from_nid);

	while (!down_read_trylock(&mm->mmap_sem)) {
		if (!tsk->at_remote && down_read_retry++ > 4) {
			res->result = VM_FAULT_RETRY;
			goto out_up;
		}
		schedule();
	}
	vma = find_vma(mm, req->addr);
	if (!vma || vma->vm_start > req->addr) {
		res->result = VM_FAULT_SIGBUS;
		goto out_up;
	}

#ifdef CONFIG_POPCORN_CHECK_SANITY
	BUG_ON(vma->vm_flags & VM_EXEC);
#endif

	if (tsk->at_remote) {
		res->result = __handle_remotefault_at_remote(tsk, mm, vma, req, res);
	} else {
		res->result = __handle_remotefault_at_origin(tsk, mm, vma, req, res);
	}
	at_remote = tsk->at_remote;

out_up:
	if (res->result != VM_FAULT_RETRY) {
		up_read(&mm->mmap_sem);
	}
	mmput(mm);
	put_task_struct(tsk);

	if (res->result == VM_FAULT_LOCKED) {
		goto again;
	}

out:
	if (res->result != 0 || TRANSFER_PAGE_WITH_RDMA) {
		res_type = PCN_KMSG_TYPE_REMOTE_PAGE_RESPONSE_SHORT;
		res_size = sizeof(remote_page_response_short_t);
	} else {
		res_type = PCN_KMSG_TYPE_REMOTE_PAGE_RESPONSE;
		res_size = sizeof(remote_page_response_t);
	}
	res->addr = req->addr;
	res->remote_pid = req->remote_pid;

	res->origin_pid = req->origin_pid;
	res->origin_ws = req->origin_ws;

	PGPRINTK("  [%d] ->[%d/%d] %x\n", req->remote_pid,
			res->origin_pid, from_nid, res->result);

	trace_pgfault(from_nid, req->remote_pid,
			fault_for_write(req->fault_flags) ? 'W' : 'R',
			req->instr_addr, req->addr, res->result);

	pcn_kmsg_post(res_type, from_nid, res, res_size);

#if PREFETCH_SUPPORT
	/* Only support remote prefetch */
	if (!at_remote)
		__prefetch_at_origin(from_nid, req->remote_pid,
					req->origin_pid, req->pf_list_size,
					(struct prefetch_body*)&req->pf_list);
#endif
	END_KMSG_WORK(req);
}


/**************************************************************************
 * Exclusively keep a user page to the current node. Should put the user
 * page after use. This routine is similar to localfault handler at origin
 * thus may be refactored.
 */
int page_server_get_userpage(u32 __user *uaddr, struct fault_handle **handle, char *mode)
{
	unsigned long addr = (unsigned long)uaddr & PAGE_MASK;
	struct mm_struct *mm;
	struct vm_area_struct *vma;

	const unsigned long fault_flags = 0;
	struct fault_handle *fh = NULL;
	spinlock_t *ptl;
	pmd_t *pmd;
	pte_t *pte;

	bool leader;
	int ret = 0;

	*handle = NULL;
	if (!distributed_process(current)) return 0;

	mm = get_task_mm(current);
retry:
	down_read(&mm->mmap_sem);
	vma = find_vma(mm, addr);
	if (!vma || vma->vm_start > addr) {
		ret = -EINVAL;
		goto out;
	}

	pte = __get_pte_at(mm, addr, &pmd, &ptl);
	if (!pte) {
		ret = -EINVAL;
		goto out;
	}
	spin_lock(ptl);
	fh = __start_fault_handling(current, addr, fault_flags, ptl, &leader);
	if (!fh) {
		pte_unmap(pte);
		up_read(&mm->mmap_sem);
		io_schedule();
		goto retry;
	}

	/*
	PGPRINTK(" %c[%d] gup %s %p %p\n", leader ? '=' : '-', current->pid, mode,
		fh, uaddr);
	*/

	if (leader && !page_is_mine(mm, addr)) {
		struct page *page = get_normal_page(vma, addr, pte);
		__claim_remote_page(current, mm, vma, addr, fault_flags, page);

		spin_lock(ptl);
		__make_pte_valid(mm, vma, addr, fault_flags, pte);
		spin_unlock(ptl);
	}
	pte_unmap(pte);
	ret = 0;

out:
	*handle = fh;
	up_read(&mm->mmap_sem);
	mmput(mm);
	return ret;
}

void page_server_put_userpage(struct fault_handle *fh, char *mode)
{
	if (!fh) return;

	__finish_fault_handling(fh);
}

/*
 * In normal page fault path. Sleeping will interfere with mm_fault time measurement.
 */
inline struct prefetch_list *alloc_prefetch_list(void)
{
	return kzalloc(sizeof(struct prefetch_list), GFP_ATOMIC);
}

inline void free_prefetch_list(struct prefetch_list* pf_list)
{
	if (pf_list) {
		kfree(pf_list);
		pf_list = NULL;
	}
}

/*
 * Maybe on normal page fault path.
 * Sleeping will interfere with mm_fault time measurement.
 */
inline void add_pf_list_at(struct prefetch_list* pf_list,
			struct pf_ongoing_map *pf_map, unsigned long addr, bool write,
				bool besteffort, int pf_nr_pages, struct fault_handle *fh)
{
	int slot_num = pf_nr_pages;
	struct prefetch_body *list_ptr = (struct prefetch_body*)pf_list;
	BUG_ON(!(pf_map));
	(pf_map)->addr[slot_num] = addr;
	(pf_map)->fh[slot_num] = fh;

	(list_ptr + slot_num)->addr = addr;
	(list_ptr + slot_num)->is_write = write;
	(list_ptr + slot_num)->is_besteffort = besteffort;

	PFPRINTK("select: [%d]-#%d %lx w %d b %d fh %p\n",
			current->pid, slot_num, addr, write, besteffort, fh);

//	if (TRANSFER_PAGE_WITH_RDMA) { //ooo
//		struct pcn_kmsg_rdma_handle *handle =
//				pcn_kmsg_pin_rdma_buffer(NULL, PAGE_SIZE);
//		if (IS_ERR(handle)) BUG();
//		(pf_map)->rh[slot_num] = handle;
//		(list_ptr + slot_num)->rdma_addr = handle->dma_addr;
//		(list_ptr + slot_num)->rdma_key = handle->rkey;
//	} else {
		(pf_map)->rh[slot_num] = NULL;
		(list_ptr + slot_num)->rdma_addr = 0;
		(list_ptr + slot_num)->rdma_key = 0;
//	}

	/* Keep this info so that origin don't need to send back the list length */
	(pf_map)->pf_list_size++;
}

inline void page_server_pf_action_record(int behavior)
{
#ifdef CONFIG_POPCORN_STAT
	if (behavior == MADV_READ) {
		pf_action_record_remote_madv_read();
	} else if (behavior == MADV_WRITE) {
		pf_action_record_remote_madv_write();
	}
#endif
}

/*
 * xxx: not released while EXIT
 * per vm
 */
long page_server_prefetch_enq(unsigned long start, unsigned long end, int behavior)
{
	int err = 0;
	struct prefetch_madvise *curr;

	/* xxx - sorry origin */
	if (!current->at_remote) return 0;

	curr = kmalloc(sizeof(*curr), GFP_KERNEL);
	curr->pid = current->pid;
	curr->start_addr = start;
	curr->end_addr = end;
	curr->offset = 0;

	if (behavior == MADV_READ) {
		curr->is_write = false;
	} else if (behavior == MADV_WRITE) {
		curr->is_write = true;
	}
#ifdef CONFIG_POPCORN_CHECK_SANITY
	else BUG();
#endif

	//curr->is_besteffort = (addr / PAGE_SIZE) % 2 ? true : false;
	curr->is_besteffort = true;
	//curr->is_besteffort = false;

	spin_lock(&current->pf_req_lock);
	list_add_tail(&curr->list, &current->pf_req_list);
	spin_unlock(&current->pf_req_lock);

	return err;
}

/*
 * In normal page fault path. Sleeping will interfere with mm_fault time measurement.
 * Get pf page info from mdivise()
 */
#if 0
bool prefetch_policy(struct prefetch_list* pf_list)
{
	int cnt = 0;
	struct prefetch_madvise *curr, *tmp;
	struct prefetch_body *list_ptr = (struct prefetch_body*)pf_list;
	if (!pf_list) return false;

	/* spining overhead << remote page fault overhead */
	spin_lock(&current->pf_req_lock);
	list_for_each_entry_safe(curr, tmp, &current->pf_req_list, list) {
#ifdef CONFIG_POPCORN_CHECK_SANITY
		if (curr->pid != current->pid) BUG();
#endif
		list_ptr->addr = curr->pfb.addr;
		list_ptr->is_write = curr->pfb.is_write;
		list_ptr->is_besteffort = curr->pfb.is_besteffort;
		list_ptr++;
		list_del(&curr->list);
		/* PFPRINTK("%s(): %d %lx w(%s) b(%s) q->l\n", __func__,
				curr->pid, curr->pfb.addr,
				curr->pfb.is_write ? "O" : "X",
				curr->pfb.is_besteffort ? "O" : "X"); */
		kfree(curr);
		cnt++;
		if (cnt >= MAX_PF_REQ) break;
	}
	spin_unlock(&current->pf_req_lock);
	if (!cnt)
		return false;
	else
		return true;
fail:
	free_prefetch_list(pf_list);
	pf_list = NULL;
	return false;
}
#endif

#if 0
void prefetch_dummy_policy(struct prefetch_list* pf_list, unsigned long fault_addr)
{
	static uint8_t cnt = 0;
	struct prefetch_body *list_ptr;
	cnt++;
	if (cnt >= PREFETCH_DURATION) {
		int i = 0;
		list_ptr = (struct prefetch_body*)pf_list;
		for(i = 0; i < PREFETCH_NUM_OF_PAGES; i++) {
			list_ptr->addr = fault_addr + ((i + SKIP_NUM_OF_PAGES) * PAGE_SIZE);
			//list_ptr->is_write = i % 2 ? true : false;
			//list_ptr->is_write = true;
			list_ptr->is_write = false;
			//list_ptr->is_besteffort = i % 2 ? true : false;
			//list_ptr->is_besteffort = true;
			list_ptr->is_besteffort = false;
			PFPRINTK("dummy policy: [%d] [%d] %lx\n", current->pid, i, list_ptr->addr);
			list_ptr++;
		}
		cnt = 0;
    }
}
#endif

/* only delete from queue, not free. The user has to free it */
/* per vm */
//
// TODO
// page_server_pf_action_record(behavior);
// some where
struct prefetch_madvise *prefetch_deq(void)
{
	bool found = false;
	struct prefetch_madvise *curr, *tmp;


	/* spining overhead << remote page fault overhead */
	spin_lock(&current->pf_req_lock);
#if 0 // test this implementation
	if (!list_empty(&current->pf_req_list)) {
		found = true;
		curr = list_first_entry(&current->pf_req_list, struct prefetch_madvise, list);
#ifdef CONFIG_POPCORN_CHECK_SANITY
		BUG_ON(!curr);
		if (curr->pid != current->pid) BUG();
#endif
		list_del(&curr->list);
	}
#endif
#if 1
	/* TODO change to container of */ //curr = list_entry(curr, typeof(*curr), list);
	list_for_each_entry_safe(curr, tmp, &current->pf_req_list, list) {
#ifdef CONFIG_POPCORN_CHECK_SANITY
		if (curr->pid != current->pid) BUG();
#endif
		/* PFPRINTK("%s(): %d %lx w(%s) b(%s) q->l\n", __func__,
				curr->pid, curr->pfb.addr,
				curr->pfb.is_write ? "O" : "X",
				curr->pfb.is_besteffort ? "O" : "X"); */
		found = true;
		list_del(&curr->list);
		break;
	}
#endif
	spin_unlock(&current->pf_req_lock);

	if (found)
		return curr;
	else
		return NULL;
}

/* Maybe on normal page fault path.
 * Sleeping will interfere with mm_fault time measurement.
 */
void prefetch_req_list_add(struct prefetch_list *pf_list,
							struct pf_ongoing_map **pf_map,
							int slot, unsigned long addr,
							bool is_write, bool is_besteffort,
							struct fault_handle *fh)
{
	if (!slot) { /* first one should create a pf ongoing mapping */
		*pf_map = kzalloc(sizeof(struct pf_ongoing_map), GFP_ATOMIC);
		BUG_ON(!(*pf_map));
		(*pf_map)->pid = current->pid;
		spin_lock(&current->pf_ongoing_lock);
		list_add_tail(&(*pf_map)->list, &current->pf_ongoing_list);
		spin_unlock(&current->pf_ongoing_lock);
	}
	add_pf_list_at(pf_list, *pf_map, addr, is_write,
					is_besteffort, slot, fh);
}

/* Maybe on normal page fault path.
 * Sleeping will interfere with mm_fault time measurement.
 */
bool select_prefetch_page(struct prefetch_madvise *curr_madvise,
						struct mm_struct *mm, int *pf_nr_pages,
						struct prefetch_list *pf_list,
						struct pf_ongoing_map **pf_map)
{
	bool is_end = false;
	int fk;
	pte_t *pte;
	pmd_t *pmd;
	spinlock_t *ptl;
	struct fault_handle *fh;
	struct remote_context *rc;
	unsigned long addr, flags;
	bool is_write, is_besteffort, found = false, leader = false;
	bool is_pte_write;

	addr = curr_madvise->start_addr + (curr_madvise->offset * PAGE_SIZE);
	curr_madvise->offset++;
	is_write = curr_madvise->is_write;
	is_besteffort = curr_madvise->is_besteffort;

	/* Release if the next one hits the end */
	if (addr + PAGE_SIZE >= curr_madvise->end_addr)
		is_end = true;

	/* Only for testing local pte permission, read/write */
	pte = __get_pte_at(mm, addr, &pmd, &ptl);
	if (!pte) {
		PFPRINTK("%s(): local skip !pte %lx\n", __func__, addr);
		pf_action_fail_record_remote_no_pte();
		goto next;
	}

	// xxx: test: removing ptl directly
//	if (is_besteffort) {
//		spin_lock(ptl);
//	} else {
//		if (!spin_trylock(ptl)) { //xxx
//			pte_unmap(pte);
//			PFPRINTK("%s(): local skip pte locked %lx\n", __func__, addr);
//			goto next;
//		}
//	}

	rc = __get_mm_remote(mm);
	fk = __fault_hash_key(addr);

	if (is_besteffort) {
		spin_lock_irqsave(&rc->faults_lock[fk], flags);
	} else {
		if (!spin_trylock_irqsave(&rc->faults_lock[fk], flags)) {
//			spin_unlock(ptl); //xxx
			pte_unmap(pte);
			PFPRINTK("%s(): local skip fh locked %lx\n", __func__, addr);
			goto next_put;
		}
	}
//	spin_unlock(ptl);

	hlist_for_each_entry(fh, &rc->faults[fk], list) {
		if (fh->addr == addr) {
			found = true;
			break;
		}
	}

	if (found) { /* found - bz */
		pf_action_fail_record_remote_busy();
	} else {
		if (!page_is_mine(mm, addr)) { /* X->R/W */ 
			fh = __alloc_fault_handle(current, addr, rc);
			pf_time_start_with_fh(fh);
			if (is_write) {
				fh->flags |= FAULT_HANDLE_WRITE;
				pf_action_record_remote_req_write_not_grant();
			} else {
				pf_action_record_remote_req_read();
			}
			leader = true;
		} else { /* R->R/W */
//			spin_lock(ptl); // deadlock
			is_pte_write = pte_write(*pte);
//			spin_unlock(ptl); // deadlock
			if (is_pte_write) {
				if (is_write) { /* W->W */
					pf_action_record_remote_req_write_not_grant();
				} else { /* W->R */
					pf_action_record_remote_req_write_not_grant();
				}
			} else { /* R->R/W */
				if (is_write) {
					fh = __alloc_fault_handle(current, addr, rc);
					pf_time_start_with_fh(fh);
					pf_action_record_remote_req_write_grant();
					fh->flags |= FAULT_HANDLE_WRITE;
					leader = true;
#ifdef CONFIG_POPCORN_CHECK_SANITY
					BUG_ON(pte_write(*pte));
#endif
				} else { /* R->R */
					pf_action_fail_record_remote_pg_is_mine();
				}
			}
		}
	}
	spin_unlock_irqrestore(&rc->faults_lock[fk], flags);
	pte_unmap(pte);

#if 0
	if (!found) {
		if (!page_is_mine(mm, addr) || (	/* I don't own OR */
			page_is_mine(mm, addr) && 		/* I own */
			!pte_write(*pte) &&				/* but read-only */
			is_write ))						/* pf w/ write */
		{
			fh = __alloc_fault_handle(current, addr, rc);
			pf_time_start_with_fh(fh);
			if (is_write)
				fh->flags |= FAULT_HANDLE_WRITE;
			leader = true;

#ifdef CONFIG_POPCORN_CHECK_SANITY
			if (page_is_mine(mm, addr) &&	/* I own */
				!pte_write(*pte) &&			/* but read-only */
				is_write ) {				/* pf w/ write */
				/* R->W */
				pf_action_record_remote_req_write_grant();
				BUG_ON(pte_write(*pte));
			} else { /* read or write + not owner */ /* page is not mine */
				if (is_write) { /* X->W */
					pf_action_record_remote_req_write_not_grant();
				}
				else /* one pg request for pf_r */
					pf_action_record_remote_req_read();
			}
#endif
		} else if ( page_is_mine(mm, addr) && !is_write ) {
			/* read req | I've owned */
			pf_action_fail_record_remote_pg_is_mine();
		}
	} else { /* found - bz */
		pf_action_fail_record_remote_busy();
	}
	spin_unlock_irqrestore(&rc->faults_lock[fk], flags);
	pte_unmap(pte);
#endif
next_put:
	__put_task_remote(rc);
	if (leader) {
		prefetch_req_list_add(pf_list, pf_map, *pf_nr_pages,
							addr, is_write, is_besteffort, fh);
		(*pf_nr_pages)++;
	}
next:
	return is_end;
}

/*
 * In normal page fault path. Sleeping will interfere with mm_fault time measurement.
 * Select prefetched pages
 * 		get madvise req from FIFO
 * 		return a preftech list
 */
struct prefetch_list *select_prefetch_pages(struct mm_struct *mm, int *pf_nr_pages)
{
	int try_cnt = 0;
	bool skip = false, is_end = false;
    struct prefetch_list *pf_list = NULL;
	struct pf_ongoing_map *pf_map = NULL;
	struct prefetch_madvise *curr_madvise;

	/* Controlling max request on the fly */
	spin_lock(&current->pf_ongoing_lock);
	if (current->pf_ongoing_cnt >= ONGOING_PF_REQ_PER_THREAD)
		skip = true;
	spin_unlock(&current->pf_ongoing_lock);
	if (skip) goto out;

//		pf_map = kzalloc(sizeof(*pf_map), GFP_ATOMIC);
//		BUG_ON(!(pf_map));
//		(pf_map)->pid = current->pid;
//		spin_lock(&current->pf_ongoing_lock);
//		list_add_tail(&(pf_map)->list, &current->pf_ongoing_list);
//		spin_unlock(&current->pf_ongoing_lock);

	curr_madvise = prefetch_deq();
	if (!curr_madvise) {
		goto out;
	}

    pf_list = alloc_prefetch_list();
	BUG_ON(!pf_list);
	while (*pf_nr_pages < MAX_PF_REQ && try_cnt < MAX_TRY_MADVISE_REQ) {
		if (select_prefetch_page(curr_madvise, mm, pf_nr_pages, pf_list, &pf_map)) {
			is_end = true;
			kfree(curr_madvise);
			break;
		}
		try_cnt++;
    }
	
	if (!is_end) { /* leftover */
		spin_lock(&current->pf_req_lock);
		list_add(&curr_madvise->list, &current->pf_req_list);
		spin_unlock(&current->pf_req_lock);
	}

	if (*pf_nr_pages) {
		spin_lock(&current->pf_ongoing_lock);
		current->pf_ongoing_cnt++;
		PFPRINTK("%d has %d ongoing pf left\n",
					current->pid, current->pf_ongoing_cnt);
		spin_unlock(&current->pf_ongoing_lock);
	} else {
//		spin_lock(&current->pf_ongoing_lock);
//		list_del(&(pf_map)->list);
//		spin_unlock(&current->pf_ongoing_lock);
//		kfree(pf_map);
	}
out:
    return pf_list;
}


/* The pf requester is holding the fh as a leader. Release fh. */
static void __pf_finish(int result, int ret,
						struct pf_ongoing_map *pf_map, int pf_num)
{
	struct fault_handle *fh = pf_map->fh[pf_num];
#ifdef CONFIG_POPCORN_CHECK_SANITY
	BUG_ON(!fh);
#endif
	fh->ret = ret; /* To followers - done(0) / retry(VM_FAULT_RETRY) */
	__finish_fault_handling(fh);
	pf_time_end_with_fh(fh, result & PREFETCH_SUCCESS);
	return;
}


/**************************************************************************
 * Local fault handler at the remote
 */
static int __handle_localfault_at_remote(struct mm_struct *mm,
		struct vm_area_struct *vma, unsigned long addr,
		pmd_t *pmd, pte_t *pte, pte_t pte_val, unsigned int fault_flags)
{
	spinlock_t *ptl;
	struct page *page;
	bool populated = false;
	struct mem_cgroup *memcg;
	int ret = 0, pf_nr_pages = 0;
	struct prefetch_list *pf_list = NULL;

	struct fault_handle *fh;
	bool leader;
	remote_page_response_t *rp;

	if (anon_vma_prepare(vma)) {
		BUG_ON("Cannot prepare vma for anonymous page");
		pte_unmap(pte);
		return VM_FAULT_SIGBUS;
	}

	ptl = pte_lockptr(mm, pmd);
	spin_lock(ptl);

	if (!pte_same(*pte, pte_val)) {
		pte_unmap_unlock(pte, ptl);
		PGPRINTK("  [%d] %lx already handled\n", current->pid, addr);
		return 0;
	}
	fh = __start_fault_handling(current, addr, fault_flags, ptl, &leader);
	if (!fh) {
		pte_unmap(pte);
		up_read(&mm->mmap_sem);
		return VM_FAULT_RETRY;
	}

	PGPRINTK(" %c[%d] %lx %p\n", leader ? '=' : '-', current->pid, addr, fh);
	if (!leader) {
		pte_unmap(pte);
		ret = fh->ret;
		if (ret) up_read(&mm->mmap_sem);
		goto out_follower;
	}

	if (pte_none(*pte) || !(page = vm_normal_page(vma, addr, *pte))) {
		page = alloc_page_vma(GFP_HIGHUSER_MOVABLE, vma, addr);
		BUG_ON(!page);

		if (mem_cgroup_try_charge(page, mm, GFP_KERNEL, &memcg)) {
			BUG();
		}
		populated = true;
	}
	get_page(page);

#if PREFETCH_SUPPORT
	pf_list = select_prefetch_pages(mm, &pf_nr_pages);
#endif

	/* TODO: think about it */
	rp = __fetch_page_from_origin(current, vma, addr,
								fault_flags, page, pf_list, pf_nr_pages);

	if (rp->result && rp->result != VM_FAULT_CONTINUE) {
		if (rp->result != VM_FAULT_RETRY)
			PGPRINTK("  [%d] failed 0x%x\n", current->pid, rp->result);
		ret = rp->result;
		pte_unmap(pte);
		up_read(&mm->mmap_sem);
		goto out_free;
	}

	if (rp->result == VM_FAULT_CONTINUE) {
		/**
		 * Page ownership is granted without transferring the page data
		 * since this node already owns the up-to-dated page
		 */
		pte_t entry;
		BUG_ON(populated); /* shouldn't be populated since it was there */

		spin_lock(ptl);
		entry = pte_make_valid(*pte);

		if (fault_for_write(fault_flags)) {
			entry = pte_mkwrite(entry);
			entry = pte_mkdirty(entry);
		} else {
			entry = pte_wrprotect(entry);
		}
		entry = pte_mkyoung(entry);

		if (ptep_set_access_flags(vma, addr, pte, entry, 1)) {
			update_mmu_cache(vma, addr, pte);
		}
	} else {
		spin_lock(ptl);
		if (populated) {
			do_set_pte(vma, addr, page, pte, fault_for_write(fault_flags), true);
			mem_cgroup_commit_charge(page, memcg, false);
			lru_cache_add_active_or_unevictable(page, vma);
		} else {
			__make_pte_valid(mm, vma, addr, fault_flags, pte);
		}
	}
	SetPageDistributed(mm, addr);
	set_page_owner(my_nid, mm, addr);
	pte_unmap_unlock(pte, ptl);
	ret = 0;	/* The leader squashes both 0 and VM_FAULT_CONTINUE to 0 */

out_free:
	put_page(page);
	pcn_kmsg_done(rp);
	fh->ret = ret;
	free_prefetch_list(pf_list);

out_follower:
	__finish_fault_handling(fh);
	return ret;
}


static bool __handle_copy_on_write(struct mm_struct *mm,
		struct vm_area_struct *vma, unsigned long addr,
		pte_t *pte, pte_t *pte_val, unsigned int fault_flags)
{
	if (vma_is_anonymous(vma) || fault_for_read(fault_flags)) return false;
	BUG_ON(vma->vm_flags & VM_SHARED);

	/**
	 * We need to determine whether the page is already cowed or not to
	 * avoid unnecessary cows. But there is no explicit data structure that
	 * bookkeeping such information. Also, explicitly tracking every CoW
	 * including non-distributed processes is not desirable due to the
	 * high frequency of CoW.
	 * Fortunately, private vma is not flushed, implying the PTE dirty bit
	 * is not cleared but kept throughout its lifetime. If the dirty bit is
	 * set for a page, the page is written previously, which implies the page
	 * is CoWed!!!
	 */
	if (pte_dirty(*pte_val)) return false;

	if (PageCowed(mm, addr)) return false;

	if (cow_file_at_origin(mm, vma, addr, pte)) return false;

	*pte_val = *pte;
	SetPageCowed(mm, addr);

	return true;
}


/**************************************************************************
 * Local fault handler at the origin
 */
static int __handle_localfault_at_origin(struct mm_struct *mm,
		struct vm_area_struct *vma, unsigned long addr,
		pmd_t *pmd, pte_t *pte, pte_t pte_val, unsigned int fault_flags)
{
	spinlock_t *ptl;

	struct fault_handle *fh;
	bool leader;

	ptl = pte_lockptr(mm, pmd);
	spin_lock(ptl);

	if (!pte_same(*pte, pte_val)) {
		pte_unmap_unlock(pte, ptl);
		PGPRINTK("  [%d] %lx already handled\n", current->pid, addr);
		return 0;
	}

	/* Fresh access to the address. Handle locally since we are at the origin */
	if (pte_none(pte_val)) {
		BUG_ON(pte_present(pte_val));
		spin_unlock(ptl);
		PGPRINTK("  [%d] fresh at origin. continue\n", current->pid);
		return VM_FAULT_CONTINUE;
	}

	/* Nothing to do with DSM (e.g. COW). Handle locally */
	if (!PageDistributed(mm, addr)) {
		spin_unlock(ptl);
		PGPRINTK("  [%d] local at origin. continue\n", current->pid);
		return VM_FAULT_CONTINUE;
	}

	fh = __start_fault_handling(current, addr, fault_flags, ptl, &leader);
	if (!fh) {
		pte_unmap(pte);
		up_read(&mm->mmap_sem);
		return VM_FAULT_RETRY;
	}

	/* Handle replicated page via the memory consistency protocol */
	PGPRINTK(" %c[%d] %lx replicated %smine %p\n",
			leader ? '=' : ' ', current->pid, addr,
			page_is_mine(mm, addr) ? "" : "not ", fh);

	if (!leader) {
		pte_unmap(pte);
		goto out_wakeup;
	}

	__handle_copy_on_write(mm, vma, addr, pte, &pte_val, fault_flags);

	if (page_is_mine(mm, addr)) {
		if (fault_for_read(fault_flags)) {
			/* Racy exit */
			pte_unmap(pte);
			goto out_wakeup;
		}

		__claim_local_page(current, addr, my_nid, mm);

		spin_lock(ptl);
		pte_val = pte_mkwrite(pte_val);
		pte_val = pte_mkdirty(pte_val);
		pte_val = pte_mkyoung(pte_val);

		if (ptep_set_access_flags(vma, addr, pte, pte_val, 1)) {
			update_mmu_cache(vma, addr, pte);
		}
	} else {
		struct page *page = vm_normal_page(vma, addr, pte_val);
		BUG_ON(!page);

		__claim_remote_page(current, mm, vma, addr, fault_flags, page);

		spin_lock(ptl);
		__make_pte_valid(mm, vma, addr, fault_flags, pte);
	}
#ifdef CONFIG_POPCORN_CHECK_SANITY
	BUG_ON(!test_page_owner(my_nid, mm, addr));
#endif
	pte_unmap_unlock(pte, ptl);

out_wakeup:
	__finish_fault_handling(fh);

	return 0;
}


/**
 * Function:
 *	page_server_handle_pte_fault
 *
 * Description:
 *	Handle PTE faults with Popcorn page replication protocol.
 *  down_read(&mm->mmap_sem) is already held when getting in.
 *  DO NOT FORGET to unmap pte before returning non-VM_FAULT_CONTINUE.
 *
 * Input:
 *	All are from the PTE handler
 *
 * Return values:
 *	VM_FAULT_CONTINUE when the page fault can be handled locally.
 *	0 if the fault is fetched remotely and fixed.
 *  ERROR otherwise
 */
int page_server_handle_pte_fault(
		struct mm_struct *mm, struct vm_area_struct *vma,
		unsigned long address, pmd_t *pmd, pte_t *pte, pte_t pte_val,
		unsigned int fault_flags)
{
	unsigned long addr = address & PAGE_MASK;
	int ret = 0;

	might_sleep();

	PGPRINTK("\n## PAGEFAULT [%d] %lx %c %lx %x %lx\n",
			current->pid, address,
			fault_for_write(fault_flags) ? 'W' : 'R',
			instruction_pointer(current_pt_regs()),
			fault_flags, pte_flags(pte_val));

	/**
	 * Thread at the origin
	 */
	if (!current->at_remote) {
		ret = __handle_localfault_at_origin(
				mm, vma, addr, pmd, pte, pte_val, fault_flags);
		goto out;
	}

	/**
	 * Thread running at a remote
	 *
	 * Fault handling at the remote side is simpler than at the origin.
	 * There will be no copy-on-write case at the remote since no thread
	 * creation is allowed at the remote side.
	 */
	if (pte_none(pte_val)) {
		/* Can we handle the fault locally? */
		if (vma->vm_flags & VM_EXEC) {
			PGPRINTK("  [%d] VM_EXEC. continue\n", current->pid);
			ret = VM_FAULT_CONTINUE;
			goto out;
		}
		if (!vma_is_anonymous(vma) &&
				((vma->vm_flags & (VM_WRITE | VM_SHARED)) == 0)) {
			PGPRINTK("  [%d] locally file-mapped read-only. continue\n",
					current->pid);
			ret = VM_FAULT_CONTINUE;
			goto out;
		}
	}

	if (!pte_present(pte_val)) {
		/* Remote page fault */
		ret = __handle_localfault_at_remote(
				mm, vma, addr, pmd, pte, pte_val, fault_flags);
		goto out;
	}

	if ((vma->vm_flags & VM_WRITE) &&
			fault_for_write(fault_flags) && !pte_write(pte_val)) {
		/* wr-protected for keeping page consistency */
		ret = __handle_localfault_at_remote(
				mm, vma, addr, pmd, pte, pte_val, fault_flags);
		goto out;
	}

	pte_unmap(pte);
	PGPRINTK("  [%d] might be fixed by others???\n", current->pid);
	ret = 0;

out:
	trace_pgfault(my_nid, current->pid,
			fault_for_write(fault_flags) ? 'W' : 'R',
			instruction_pointer(current_pt_regs()), addr, ret);

	return ret;
}



bool async_prefetch_request(struct mm_struct *mm, struct task_struct *tsk)
{
	remote_prefetch_request_t *req;
	bool ret = false;
	int size;
	int pf_nr_pages = 0;
	struct prefetch_list *pf_list = NULL;
	if (!mm) return ret;
	if (!tsk) return ret;

	ret = true;
	req = pcn_kmsg_get(sizeof(*req));
	req->origin_pid = tsk->pid; //rc->remote_tgids[nid];
	req->remote_pid = tsk->origin_pid; //from_pid; //tsk->pid
	
	pf_list = select_prefetch_pages(mm, &pf_nr_pages);

	if (pf_list && pf_nr_pages > 0) {
		req->pf_list_size = pf_nr_pages;
		memcpy(&req->pf_list, pf_list, sizeof(struct prefetch_list));
		size = sizeof(*req);
	} else {
		req->pf_list_size = 0;
		size = sizeof(*req) - sizeof(struct prefetch_list);
	}

	pcn_kmsg_post(PCN_KMSG_TYPE_REMOTE_PREFETCH_REQUEST,
							tsk->origin_nid, req, size);
	mmput(mm);
	put_task_struct(tsk);
	return ret;
}


/* prefetch response event handler */
static void process_remote_prefetch_response(struct work_struct *work)
{
	START_KMSG_WORK(remote_prefetch_response_t, res, work);
	bool found = false;
    struct vm_area_struct *vma;
    struct mm_struct *mm = NULL;
    struct task_struct *tsk = NULL;
	int pf_num = 0, succ_pf_num = 0;
	struct pf_ongoing_map *curr = NULL, *tmp;
	unsigned long first_addr = res->addr[0];
#ifdef CONFIG_POPCORN_CHECK_SANITY
	BUG_ON(!res);
#endif

	tsk = __get_task_struct(res->origin_pid);
    if (!tsk) goto pf_done;

	mm = get_task_mm(tsk);
    if (!mm) goto pf_done;

	down_read(&mm->mmap_sem);
	vma = find_vma(mm, first_addr);
	BUG_ON(!vma || vma->vm_start > first_addr);

	/* Using this pf_map, we no longer need rc.
	   It can carry more info like rdma_info as well. */
	spin_lock(&tsk->pf_ongoing_lock);
	list_for_each_entry_safe(curr, tmp, &tsk->pf_ongoing_list, list) {
		if ( curr->pid == res->origin_pid &&
			curr->addr[0] == first_addr) { /* use the fist requesting addr to locate the map */
			found = true;
			list_del(&curr->list);
			break;
		}
	}
	spin_unlock(&tsk->pf_ongoing_lock);

#ifdef CONFIG_POPCORN_CHECK_SANITY
	if (!found) { /* do_exit() has cleaned everything (not yet implemented) */
//		printk("%d %d write(%s) %lx VM_FAULT_CONTINUE(%s) SUCC(%s)\n",
//				res->origin_pid, res->remote_pid,
//				is_write ? "O" : "X", addr,
//				result & VM_FAULT_CONTINUE ? "O" : "X",
//				result & PREFETCH_SUCCESS ? "O" : "X");
		BUG(); /* waiting for the implementation in do_exit() */
		goto pf_done;
	}
#endif

	PFPRINTK("response: [%d] [%d]\n",
			tsk->pid, curr->pf_list_size);

#ifdef CONFIG_POPCORN_CHECK_SANITY
	BUG_ON(!curr->pf_list_size);
#endif
	while (pf_num < MAX_PF_REQ && pf_num < curr->pf_list_size) {
		pte_t *pte;
		pmd_t *pmd;
		void *paddr;
		spinlock_t *ptl;
		struct page *page;
		struct mem_cgroup *memcg;
		int ret = VM_FAULT_RETRY;	/* If prefetch fail, followers retry */
		bool populated = false;
		unsigned long addr = res->addr[pf_num];
		bool is_write = res->is_write[pf_num];
		int result = res->result[pf_num];
		char* res_page = &res->page[succ_pf_num][0];

#ifdef CONFIG_POPCORN_CHECK_SANITY
		if (page_is_mine(mm, addr) && !is_write) BUG();
						/* if, concurrency problem */
#endif

		if (!(result & PREFETCH_SUCCESS)) {
			if (!is_write)
				pf_action_record_remote_res_fail_read();
			else
				pf_action_record_remote_res_fail_write();
			goto next;
		}

		succ_pf_num++;

		/* refet to localfault_at_remote */
		pte = __get_pte_at(mm, addr, &pmd, &ptl);
		if (!pte) {
			PGPRINTK("  [%d] No PTE!!\n", tsk->pid);
			BUG();
		}

		/* get a page frame for the vma page if needed */
		if (pte_none(*pte) ||
			!(page = vm_normal_page(vma, addr, *pte))) {
			page = alloc_page_vma(GFP_HIGHUSER_MOVABLE, vma, addr);
			mem_cgroup_try_charge(page, mm, GFP_KERNEL, &memcg);
			populated = true;
			PFPRINTK("recv:\t\tpopulated(%s) %lx=====================\n",
						populated ? "O" : "X", addr);
		}
		get_page(page);

		/* update pte - two cases:	1. prefetch not up-to-date
									2. prefetch w/ up-to-date	*/
		if (result & VM_FAULT_CONTINUE) {
			/* bottom-half of localfault_at_remote */
			/**
			 * Page ownership is granted without transferring the page data
			 * since this node already owns the up-to-dated page
			 */
			pte_t entry;
			BUG_ON(populated); /* shouldn't be populated since it was there */
#ifdef CONFIG_POPCORN_CHECK_SANITY
			BUG_ON(!page_is_mine(mm, addr));
#endif
			pf_action_record_remote_res_succ_write_grant();
			PFPRINTK("recv:\t\t preown & up-dto-date %lx\n", addr);

			spin_lock(ptl);
			entry = pte_make_valid(*pte);

			if (fault_for_write(is_write ? FAULT_FLAG_WRITE : 0)) {
				entry = pte_mkwrite(entry);
				entry = pte_mkdirty(entry);
			} else {
				entry = pte_wrprotect(entry);
			}
			entry = pte_mkyoung(entry);

			if (ptep_set_access_flags(vma, addr, pte, entry, 1)) {
				update_mmu_cache(vma, addr, pte);
			}
		} else {
			/* refer to __fetch_page_from_origin() */
			paddr = kmap(page);
//			if (TRANSFER_PAGE_WITH_RDMA) { //ooo
//				BUG_ON(!curr->rh);
//				copy_to_user_page(vma, page, addr, paddr, curr->rh->addr, PAGE_SIZE);
//				pcn_kmsg_unpin_rdma_buffer(curr->rh);
//			} else {
			   copy_to_user_page(vma, page, addr, paddr, res_page, PAGE_SIZE);
//       	}
			kunmap(page);
			__SetPageUptodate(page);

			/* bottom-half of localfault_at_remote */
			spin_lock(ptl);
			if (populated) {
				do_set_pte(vma, addr, page, pte, is_write, true);
				mem_cgroup_commit_charge(page, memcg, false);
				lru_cache_add_active_or_unevictable(page, vma);
				pf_action_record_remote_res_succ_write_grant();
				PFPRINTK("recv:\t\t not preown %lx\n", addr);
			} else {
				unsigned fault_flags = is_write ? FAULT_FLAG_WRITE : 0;
				__make_pte_valid(mm, vma, addr, fault_flags, pte);
				if (fault_flags)
					pf_action_record_remote_res_succ_write_not_grant();
				else
					pf_action_record_remote_res_succ_read();
				PFPRINTK("recv:\t\t preown but not CONTINUE(up-to-date) %lx\n", addr);
			}
		}

		SetPageDistributed(mm, addr);
		set_page_owner(my_nid, mm, addr);

		pte_unmap_unlock(pte, ptl);
		put_page(page);

		ret = 0; /* PREFETCH_SUCCESS */
next:
		PFPRINTK("recv: %p #%d >%lx (%s) fh %p [%d]\n", curr, pf_num, addr,
				result & PREFETCH_SUCCESS ? "O" : "X", curr->fh[pf_num], curr->pf_list_size);
		__pf_finish(result, ret, curr, pf_num);
		pf_num++;
	}
	up_read(&mm->mmap_sem);
	kfree(curr);

pf_done:
	if (mm) mmput(mm);
	if (tsk) put_task_struct(tsk);
    END_KMSG_WORK(res);

	//if (async_prefetch_request(mm, tsk)) //real_prefetching(); recycling mm and tsk
	//	tsk->pf_ongoing_cnt--;
}

/**************************************************************************
 * Routing popcorn messages to workers
 */
DEFINE_KMSG_WQ_HANDLER(remote_page_request);
DEFINE_KMSG_WQ_HANDLER(page_invalidate_request);
DEFINE_KMSG_WQ_HANDLER(remote_prefetch_request);
DEFINE_KMSG_WQ_HANDLER(remote_prefetch_response);
DEFINE_KMSG_ORDERED_WQ_HANDLER(remote_page_flush);

int __init page_server_init(void)
{
	REGISTER_KMSG_WQ_HANDLER(
			PCN_KMSG_TYPE_REMOTE_PAGE_REQUEST, remote_page_request);
	REGISTER_KMSG_HANDLER(
			PCN_KMSG_TYPE_REMOTE_PAGE_RESPONSE, remote_page_response);
	REGISTER_KMSG_HANDLER(
			PCN_KMSG_TYPE_REMOTE_PAGE_RESPONSE_SHORT, remote_page_response);
	REGISTER_KMSG_WQ_HANDLER(
			PCN_KMSG_TYPE_PAGE_INVALIDATE_REQUEST, page_invalidate_request);
	REGISTER_KMSG_HANDLER(
			PCN_KMSG_TYPE_PAGE_INVALIDATE_RESPONSE, page_invalidate_response);
	REGISTER_KMSG_WQ_HANDLER(
			PCN_KMSG_TYPE_REMOTE_PAGE_FLUSH, remote_page_flush);
	REGISTER_KMSG_WQ_HANDLER(
			PCN_KMSG_TYPE_REMOTE_PAGE_RELEASE, remote_page_flush);
	REGISTER_KMSG_HANDLER(
			PCN_KMSG_TYPE_REMOTE_PAGE_FLUSH_ACK, remote_page_flush_ack);
    REGISTER_KMSG_WQ_HANDLER(
		PCN_KMSG_TYPE_REMOTE_PREFETCH_REQUEST, remote_prefetch_request);
    REGISTER_KMSG_WQ_HANDLER(
		PCN_KMSG_TYPE_REMOTE_PREFETCH_RESPONSE, remote_prefetch_response);

	__fault_handle_cache = kmem_cache_create("fault_handle",
			sizeof(struct fault_handle), 0, 0, NULL);

#ifdef CONFIG_POPCORN_STAT
	atomic64_set(&pf_cnt, 0);
	atomic64_set(&pf_time_u, 0);

	atomic64_set(&pf_fail_cnt, 0);
	atomic64_set(&pf_fail_time_u, 0);
	atomic64_set(&pf_succ_cnt, 0);
	atomic64_set(&pf_succ_time_u, 0);
#endif
#ifdef CONFIG_POPCORN_STAT_PGFAULTS
	atomic64_set(&mm_cnt, 0);
	atomic64_set(&mm_time_u, 0);
#endif
	return 0;
}
