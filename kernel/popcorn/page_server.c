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

//extern int prefetch_at_origin(remote_page_request_t *req);
//#define PFPRINTK(...) printk(__VA_ARGS__)
//#define PFPRINTK(...) printk(KERN_INFO __VA_ARGS__)
#define PFPRINTK(...)
#define PREFETCH_SUPPORT 1
#define PREFETCH_REMOTE_REMOTE_IMM 0

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

struct remote_remote_prefetch_req {
	struct list_head list;
	unsigned long addr;
	unsigned long flags;
};

#ifdef CONFIG_POPCORN_STAT
enum pf_action_code {
	/* MADV */
	PF_READ_MADV = 0,
	PF_WRITE_MADV,
	NONE,					// -1

	/* REQ */
	PF_READ_REQ = 3, // redundant
	PF_WRITE_X_REQ, // redundant
	PF_WRITE_O_REQ, // redundant

	/* SUCC */
	PF_READ_RES_SUCCESS = 6,
	PF_WRITE_X_RES_SUCCCESS,
	PF_WRITE_O_RES_SUCCCESS,

	/* FAIL */
	PF_READ_RES_FAIL = 9,
	PF_WRITE_X_RES_FAIL,	// = PF_WRITE_RES_FAIL
	PF_WRITE_O_RES_FAIL,	// xxx + => in 10

	/* REMOTE FAIL */
	REMOTE_PF_FAIL_NO_VMA = 12,
	REMOTE_PF_FAIL_NO_PTE,
	REMOTE_PF_FAIL_BUSY,

	/* ORIGIN FAIL */
	ORIGIN_PF_FAIL_NO_VMA = 15,
	ORIGIN_PF_FAIL_BUSY,
	ORIGIN_PF_FAIL_REMOTE,

	/* RELEASE */
	PF_REALEASE_MADV = 18, // redundant
	PF_RELEASE_RES_SUCCESS,
	PF_RELEASE_RES_FAIL,

	PF_ACTION_TYPE_MAX
};
static unsigned long __pf_action_stat[PF_ACTION_TYPE_MAX] = { 0 }; /* xxx atomic */

const char *pf_action_type_name[PF_ACTION_TYPE_MAX] = {
    [PF_READ_MADV] = "MADV",
    [PF_READ_REQ] = "REQ", // redundant
	[PF_READ_RES_SUCCESS] = "SUCC",
	[PF_READ_RES_FAIL] = "FAIL",
	[REMOTE_PF_FAIL_NO_VMA] = "FAIL R",
	[ORIGIN_PF_FAIL_NO_VMA] = "FAIL O",
	[PF_REALEASE_MADV] = "RELEASE",
};
#endif

void pf_action_stat(struct seq_file *seq, void *v) {
#ifdef CONFIG_POPCORN_STAT
    int i, k = 3;
	if (seq) 
		seq_printf(seq, "%2s  %-12s   %2s  %-12s   %2s  %-12s\n",
							"", "R", "", "WX", "", "WO");
    for (i = 0; i < ARRAY_SIZE(__pf_action_stat) / k; i++) {
        if (seq) {
            seq_printf(seq, "%2d  %-12lu   %2d  %-12lu   %2d  %-12lu   %s\n",
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

/* for pf read/write */
inline void pf_action_record(bool r, bool m_q, //bool q_l,
						bool w_x, bool w_o,
						bool res, bool success)
{
#ifdef CONFIG_POPCORN_STAT
	if (!res) {
		if (m_q) {
			if (r)
				__pf_action_stat[PF_READ_MADV]++;
			else
				__pf_action_stat[PF_WRITE_MADV]++;
		} else {
			if (r)
				__pf_action_stat[PF_READ_REQ]++;
			else if (w_x)
				__pf_action_stat[PF_WRITE_X_REQ]++;
			else if (w_o)
				__pf_action_stat[PF_WRITE_O_REQ]++;
			else BUG();
		}
	} else {
		if (success) {
			if (r)
				__pf_action_stat[PF_READ_RES_SUCCESS]++;
			else if (w_x)
				__pf_action_stat[PF_WRITE_X_RES_SUCCCESS]++; // 0x1x11
			else if (w_o)
				__pf_action_stat[PF_WRITE_O_RES_SUCCCESS]++; // 0x0111
			else BUG();
		} else {
			if (r)
				__pf_action_stat[PF_READ_RES_FAIL]++;
			else if (w_x)
				__pf_action_stat[PF_WRITE_X_RES_FAIL]++;
			else if (w_o)
				__pf_action_stat[PF_WRITE_O_RES_FAIL]++;
			else BUG();
		}
	}
#endif
}
inline void pf_fail_action_record(bool remote, bool vma,
						bool pte, bool busy, bool pg_is_mine)
{
	if (remote) {
		if(!vma)
			__pf_action_stat[REMOTE_PF_FAIL_NO_VMA]++;
		else if (!pte)
			__pf_action_stat[REMOTE_PF_FAIL_NO_PTE]++;
		else if (busy)
			__pf_action_stat[REMOTE_PF_FAIL_BUSY]++;
		else BUG();
	}else {
		if(!vma)
			__pf_action_stat[ORIGIN_PF_FAIL_NO_VMA]++;
		else if (busy)
			__pf_action_stat[ORIGIN_PF_FAIL_BUSY]++;
		else if (!pg_is_mine)
			__pf_action_stat[ORIGIN_PF_FAIL_REMOTE]++;
		else BUG();
	}
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

/* for recording pf time on remote sides */
/* find a way to mapp (addr) */
inline void pf_time_start_fh_path(struct fault_handle *fh)
{
#ifdef CONFIG_POPCORN_STAT
	do_gettimeofday(&fh->tv_start);
	atomic64_inc(&pf_cnt);
#endif
}

inline void pf_time_end_fh_path(struct fault_handle *fh, bool succ)
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
		seq_printf(seq, "%2s  %5llu,%-6llu   %3s %-12llu   %2s  %-12s\n",
						"mm", (u64)atomic64_read(&mm_time_u) / MILLISECOND,
								(u64)atomic64_read(&mm_time_u) % MILLISECOND,
						"per", atomic64_read(&mm_cnt) ? (u64)atomic64_read(&mm_time_u) / atomic64_read(&mm_cnt) : 0, "us", "(unit)");
		seq_printf(seq, "%2s  %5llu,%-6llu   %3s %-12llu   %2s  %-12llu   %2s  %-12llu\n",
						"pf", (u64)atomic64_read(&pf_time_u) / MILLISECOND,
								(u64)atomic64_read(&pf_time_u) % MILLISECOND,
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

struct prefetch_list *alloc_prefetch_list(void);
void free_prefetch_list(struct prefetch_list* pf_list);
bool prefetch_policy(struct prefetch_list* pf_list);
void prefetch_dummy_policy(struct prefetch_list* pf_list, unsigned long fault_addr);
struct prefetch_list *select_prefetch_pages(
					struct prefetch_list* pf_list, struct mm_struct *mm, int *num);
int prefetch_at_origin(remote_page_request_t *req);

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
	if (!rc) return NULL;

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

	spin_lock_irqsave(&rc->faults_lock[fk], flags); /* ask unlock(ptl) */
	hlist_for_each_entry(fh, &rc->faults[fk], list) {
		if (fh->addr == addr) {
			PGPRINTK("  [%d] %s %s ongoing, wait\n", tsk->pid,
				fh->flags & FAULT_HANDLE_REMOTE ? "remote" : "local",
				fh->flags & FAULT_HANDLE_WRITE ? "write" : "read");
			BUG_ON(fh->flags & FAULT_HANDLE_INVALIDATE); // test this means coalesce
			fh->flags |= FAULT_HANDLE_INVALIDATE;
			fh->complete = &complete;
			found = true;
			/* TODO jack: coalse with RELEASE? or rety
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
		if (atomic_read(&fh->pendings) > 0) {
			PGPRINTK(" >[%d] %lx %p\n", fh->pid, fh->addr, fh);
#ifndef CONFIG_POPCORN_DEBUG_PAGE_SERVER
			wake_up_all(&fh->waits);
#else
			wake_up(&fh->waits);
#endif
		} else {
			PFPRINTK("has been released!!!!\n");
		}
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

static struct fault_handle *__start_releasing(struct task_struct *tsk, unsigned long addr, spinlock_t *ptl, bool *leader)
{
	unsigned long flags;
	struct remote_context *rc = get_task_remote(tsk);
	struct fault_handle *fh = NULL;
	bool found = false;
	int fk = __fault_hash_key(addr);

	rc = get_task_remote(tsk);
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
		*leader = true;
		fh = __alloc_fault_handle(tsk, addr, rc); /* xxx coales inv req */
		fh->flags = FAULT_HANDLE_RELEASE;
	}
	spin_unlock_irqrestore(&rc->faults_lock[fk], flags);
	put_task_remote(tsk);
	return fh;
}

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
 */
//
//madvis() at remote	(1pg)
//						->
//							*here (origin)
//						<-
int process_madvise_release_from_remote(vma_op_request_t *req,
			int from_nid,
			unsigned long addr,
			unsigned long end)
{
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
	if (!tsk) goto ffail;

	mm = get_task_mm(tsk);
	if(!mm) {
		put_task_struct(tsk);
		goto ffail;
	}

	/* Origin didn't hold yet. Remote has hold at madvise() */
	down_read(&mm->mmap_sem);
	vma = find_vma(mm, addr);
    BUG_ON(!vma || vma->vm_start > addr || vma->vm_end < addr);

	pte = __get_pte_at(mm, addr, &pmd, &ptl);
	if (!pte) goto put;

	spin_lock(ptl);

	if (pte_none(*pte) || !pte_present(*pte)) {
		spin_unlock(ptl);
		goto put_fail;
	}

	fh = __start_releasing(tsk, addr, ptl, &leader);

	if(!leader) {
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
ffail:
	PFPRINTK("(%s)handle RELEASE req at ori:  [%d] %d %d / %ld %lx-%lx\n",
			fail?"X":"O",
			req->remote_pid , from_nid,
			nr_pages, (end - addr) / PAGE_SIZE, addr, end);
	return fail;
}

//at_origin
//lock
//remote (success done) -> at_remote (only revoke request node)
//*origin (revoke my local)
//unlock
/* local update */

// claim_local_page
// __revoke_page_ownership PCN_KMSG_TYPE_PAGE_INVALIDATE_REQUEST
//
//
// 		process_page_invalidate_request (must done)
//			__do_invalidate_page							!!!!!!!!!!!!!!!
// 			<-
// handle_page_invalidate_response
// complete

/*page must be mine since origin doone*/
int release_page_ownership_at_local(struct vm_area_struct *vma, unsigned long addr)
{
	struct mm_struct *mm = vma->vm_mm;
	pmd_t *pmd;
	pte_t *pte;
	pte_t pte_val;
	spinlock_t *ptl;

	pte = __get_pte_at(mm, addr, &pmd, &ptl);
	BUG_ON(!pte); 	// test no pte otherwise handle it

	spin_lock(ptl);

#ifdef CONFIG_POPCORN_CHECK_SANITY
	BUG_ON(pte_none(*pte));			/* no pg frame (can ignore) */
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
 * Hanlde releasing a singl addr request form madvise
 * return err: 1/0
 */
int vma_server_madvise_remote(unsigned long start, size_t len, int behavior);
int page_server_release_page_ownership(struct vm_area_struct *vma,
												unsigned long addr)
{
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

	spin_lock(ptl);

	/* !present means I'm not the owner as well aka has been invalidated */
	if (pte_none(*pte) || !pte_present(*pte)) {
		pte_unmap_unlock(pte, ptl);
		return -1;
	}

	fh = __start_releasing(current, addr, ptl, &leader);

	if(!leader) {
		goto fail;
	}

	pf_action_release_record(true, false);
	if (current->at_remote) {
		/* 1. Remote */
		err = vma_server_madvise_remote(addr, PAGE_SIZE, MADV_RELEASE);
							// -> process_madvise_release_from_remote
							// __start_fault_handling
							// problem with prefetch_at_origin O
							// vma_server_madvise_remote()
							//at remote             			at origin
							//*remote (check if only onwer)
							//__delegate_vma_op
							//			PCN_KMSG_TYPE_VMA_OP_REQUEST
							//res = wait_at_station(ws);
							//          		-> process_vma_op_request
							//             		 	__process_vma_op_at_remote (not implemented)
							//              		__process_vma_op_at_origin
							//                  		VMA_OP_MADVISE
							//                  		[ret] = process_madvise_release_from_remote
							//          		<-__reply_vma_op
							//res
							//[return ret]
		if (err) {
			pf_action_release_record(false, false);
			goto fail;
		}
		/* 2. Local Origin - lock*/ // prolem with select_prefetch_pages()
		release_page_ownership_at_local(vma, addr); /* local */ /*page must be mine since origin doone*/
		ret = 0;
		pf_action_release_record(false, true);
	} else {
		err = 9;
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

static int __request_remote_page(struct task_struct *tsk, int from_nid, pid_t from_pid, unsigned long addr, unsigned long fault_flags, int ws_id, struct pcn_kmsg_rdma_handle **rh, struct prefetch_list* pf_list, int num)
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

	if (pf_list && num > 0) {
		req->is_pf_list = num;
		memcpy(&req->pf_list, pf_list, sizeof(struct prefetch_list)); // problem: too large
		/* xxx: optimize msg size */
		//memcpy(&req->pf_list, pf_list, num * sizeof(struct prefetch_body)); // xxx crash origin begins only on bare not vm
		//size = sizeof(*req) - sizeof(struct prefetch_list) +
		//					(num * sizeof(struct prefetch_body));
	} else {
		req->is_pf_list = 0;
		//memset(&req->pf_list, 0, sizeof(unsigned long)); /* in case */
		/* xxx: optimize msg size */
		//size = sizeof(*req) - sizeof(struct prefetch_list);
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
						from_nid, req, sizeof(*req));
						/* xxx: ??? ask SOMEHOW TIHS SIZE LARGER IS WAY FASTER !!!!!!!!!!!*/
						/* xxx: optimize msg size */
						//from_nid, req, size); /* xxx problem slowdown */
	return 0;
}

static remote_page_response_t *__fetch_page_from_origin(struct task_struct *tsk, struct vm_area_struct *vma, unsigned long addr, unsigned long fault_flags, struct page *page, struct prefetch_list* pf_list, int num)
{
	remote_page_response_t *rp;
	struct wait_station *ws = get_wait_station(tsk);
	struct pcn_kmsg_rdma_handle *rh;

	__request_remote_page(tsk, tsk->origin_nid, tsk->origin_pid,
						addr, fault_flags, ws->id, &rh, pf_list, num);

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
	int nid;
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
	pcn_kmsg_done(rp);

	if (rh) pcn_kmsg_unpin_rdma_buffer(rh);
	__put_task_remote(rc);
	kunmap(pip);
	return rp->result;
}

static void __claim_local_page(struct task_struct *tsk, unsigned long addr, int except_nid, struct mm_struct *mm)
{
	int peers;
	unsigned long offset;
	unsigned long *pi;
	struct page *pip = __get_page_info_page(mm, addr, &offset);
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
		struct remote_context *rc = get_task_remote_with_mm(mm);
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
	BUG_ON(!page);

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
	remote_page_response_t *res;
	int from_nid = PCN_KMSG_FROM_NID(req);
	struct task_struct *tsk;
	struct mm_struct *mm;
	struct vm_area_struct *vma;
	int res_size;
	enum pcn_kmsg_type res_type;
	int down_read_retry = 0;

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

	/* xxx: problem - leader and followers have their own list */
	/* xxx: only support remote prefetch */
#if PREFETCH_SUPPORT
	if (!tsk->at_remote)
		prefetch_at_origin(req);
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
	int ret = 0, num = 0;
//	struct timeval tv_start;
	struct prefetch_list *pf_list;
	struct prefetch_list *new_pf_list = NULL;

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
		ret = fh->ret;	// check rp->result
		if (ret) up_read(&mm->mmap_sem); // ask xxx ??? -1
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

//	pf_time_start_at_remote(&tv_start);
#if PREFETCH_SUPPORT
	pf_list = alloc_prefetch_list();
    if(prefetch_policy(pf_list))
		new_pf_list = select_prefetch_pages(pf_list, mm, &num);
		//prefetch_dummy_policy(pf_list, addr);
//	else
//		pf_time_end_at_remote(&tv_start);
#endif

	rp = __fetch_page_from_origin(current, vma, addr,
								fault_flags, page, new_pf_list, num);

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
	set_page_owner(my_nid, mm, addr); /* xxx bug at remote rw no release best */
	pte_unmap_unlock(pte, ptl);
	ret = 0;	/* The leader squashes both 0 and VM_FAULT_CONTINUE to 0 */

out_free:
	put_page(page);
	pcn_kmsg_done(rp);
	fh->ret = ret;

out_follower:
	free_prefetch_list(new_pf_list);
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

inline struct prefetch_list *alloc_prefetch_list(void)
{
	return kzalloc(sizeof(struct prefetch_list), GFP_KERNEL);
}

inline void free_prefetch_list(struct prefetch_list* pf_list)
{
	if (pf_list) kfree(pf_list);
}

inline void add_pf_list_at(struct prefetch_list* pf_list, unsigned long addr,
							bool write, bool besteffort, int slot_num)
{
	struct prefetch_body *list_ptr = (struct prefetch_body*)pf_list;
	(list_ptr + slot_num)->addr = addr;
	(list_ptr + slot_num)->is_write = write;
	(list_ptr + slot_num)->is_besteffort = besteffort;
}


/*
 * xxx: not released while EXIT
 */
long page_server_prefetch_enq(unsigned long addr, int behavior)
{
	int err = 0;
	struct prefetch_madvise *curr;

	/* xxx - sorry origin */
	if (!current->at_remote) return 0;

	curr = kmalloc(sizeof(*curr), GFP_KERNEL);
	curr->pid = current->pid;
	curr->pfb.addr = addr;

	if (behavior == MADV_READ) {
		curr->pfb.is_write = false;
		pf_action_record(true, true, false, false, false, false);
	} else if (behavior == MADV_WRITE) {
		curr->pfb.is_write = true;
		pf_action_record(false, true, false, false, false, false);
	}
#ifdef CONFIG_POPCORN_CHECK_SANITY
	else BUG();
#endif

	//curr->pfb.is_besteffort = (addr / PAGE_SIZE) % 2 ? true : false;
	curr->pfb.is_besteffort = true;
	//curr->pfb.is_besteffort = false;

	//PFPRINTK("%s(): %d %lx w(%s) b(%s) m->q\n", __func__,
	//		curr->pid, curr->pfb.addr,
	//		curr->pfb.is_write ? "O" : "X",
	//		curr->pfb.is_besteffort ? "O" : "X");

	/* xxx: optimize pf_lock */
	spin_lock(&current->pf_lock);
	list_add_tail(&curr->list, &current->pf_list);
	spin_unlock(&current->pf_lock);

	return err;
}

/*
 * In critical path.
 * Get pf page info from mdivise()
 */
bool prefetch_policy(struct prefetch_list* pf_list)
{
	int cnt = 0;
	struct prefetch_madvise *curr, *next;
	struct prefetch_body *list_ptr = (struct prefetch_body*)pf_list;
	if (!pf_list) return false;

	if (!spin_trylock(&current->pf_lock)) goto fail;
	//spin_lock(&current->pf_lock);
	if (list_empty(&current->pf_list)) {
		spin_unlock(&current->pf_lock);
		goto fail;
	}
	list_for_each_entry_safe(curr, next, &current->pf_list, list) {
#ifdef CONFIG_POPCORN_CHECK_SANITY
		if (curr->pid != current->pid) BUG(); /* impossible */
#endif
		list_ptr->addr = curr->pfb.addr;
		list_ptr->is_write = curr->pfb.is_write;
		list_ptr->is_besteffort = curr->pfb.is_besteffort;
		list_ptr++;
		list_del(&curr->list);
		//PFPRINTK("%s(): %d %lx w(%s) b(%s) q->l\n", __func__,
		//		curr->pid, curr->pfb.addr,
		//		curr->pfb.is_write ? "O" : "X",
		//		curr->pfb.is_besteffort ? "O" : "X");
		kfree(curr);
		cnt++;
		if (cnt >= MAX_PF_REQ) break;
	}
	spin_unlock(&current->pf_lock);
	return true;
fail:
	free_prefetch_list(pf_list);
	pf_list = NULL;
	return false;
}

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

/*
 * Select prefetched pages
 * 		peek existing preftech_list
 * 		return a new preftechlist
 */
struct prefetch_list *select_prefetch_pages(
        struct prefetch_list* pf_list, struct mm_struct *mm, int *num)
{
    int slot = 0, cnt = 0;
    struct prefetch_list *new_pf_list = NULL;
    struct prefetch_body *list_ptr = (struct prefetch_body*)pf_list;
    if (!pf_list) return NULL;
    if (!(list_ptr->addr)) goto out;

    new_pf_list = alloc_prefetch_list();
    while (list_ptr->addr && mm) {
		int fk;
		pte_t *pte;
		pmd_t *pmd;
		spinlock_t *ptl;
		bool found = false;
		unsigned long flags;
		struct fault_handle *fh;
        struct remote_context *rc;
        unsigned long addr = list_ptr->addr;
        struct vm_area_struct *vma = find_vma(mm, addr);
        if (!vma || vma->vm_start > addr || vma->vm_end < addr) {
			PFPRINTK("%s(): %lx > [!%lx] > %lx\n", __func__,
							vma->vm_start, addr, vma->vm_end);
			pf_fail_action_record(true, false, false, false, false);
			goto next;
		}
        pte = __get_pte_at(mm, addr, &pmd, &ptl);
		if (!pte) {
			PFPRINTK("%s(): local skip !pte %lx\n", __func__, addr);
			pf_fail_action_record(true, true, false, false, false);
			goto next;
		}

		if (list_ptr->is_besteffort) {
			spin_lock(ptl);
		} else {
			if(!spin_trylock(ptl)) {
				pte_unmap(pte);
				PFPRINTK("%s(): local skip pte locked %lx\n", __func__, addr);
				goto next;
			}
		}

		rc = get_task_remote_with_mm(mm);
        fk = __fault_hash_key(addr);

		if (list_ptr->is_besteffort) {
			spin_lock_irqsave(&rc->faults_lock[fk], flags);
		} else {
			if(!spin_trylock_irqsave(&rc->faults_lock[fk], flags)) {
				spin_unlock(ptl);
				pte_unmap(pte);
				PFPRINTK("%s(): local skip fh locked %lx\n", __func__, addr);
				goto next_put;
			}
		}
        spin_unlock(ptl);

		hlist_for_each_entry(fh, &rc->faults[fk], list) {
			if (fh->addr == addr) {
				found = true;
				break;
			}
		}

		if (!found) {
			*num += 1;
			if (!page_is_mine(mm, addr) || (	/* I don't own OR */
				page_is_mine(mm, addr) && 		/* I own */
				!pte_write(*pte) &&				/* but read-only */
				list_ptr->is_write )			/* pf w/ write */
				) {
				fh = __alloc_fault_handle(current, addr, rc);
				pf_time_start_fh_path(fh);
				if (list_ptr->is_write)
					fh->flags |= FAULT_HANDLE_WRITE;

				add_pf_list_at(new_pf_list, addr, list_ptr->is_write,
									list_ptr->is_besteffort, slot);

#ifdef CONFIG_POPCORN_CHECK_SANITY
				if (page_is_mine(mm, addr) &&	/* I own */
					!pte_write(*pte) &&			/* but read-only */
					list_ptr->is_write ) {		/* pf w/ write */
					PFPRINTK("%s(): 1 r R->W. ", __func__);
					pf_action_record(false, false, false, true, false, false); // own | w
					BUG_ON(pte_write(*pte));
				} else { /* read or write + not owner */
					if (list_ptr->is_write) {
						pf_action_record(false, false, true, false, false, false); // !own | w
						PFPRINTK("%s(): 2 r X->W. ", __func__);
					}
					else
						pf_action_record(true, false, false, false, false, false); // !own | read
				}
#endif
				PFPRINTK("select: [%d] [%d] %lx\n", current->pid, slot, addr);
				/* xxx: counter like trace_pgfault */
				slot++;
        	}
		} else { /* found */
			pf_fail_action_record(true, true, true, true, false);
		}
		pte_unmap(pte);
		spin_unlock_irqrestore(&rc->faults_lock[fk], flags);

next_put:
		__put_task_remote(rc); /* cannot use current since EXIT problem */
next:
        list_ptr++;
		cnt++;
		if (cnt >= MAX_PF_REQ) break;
    }
	if (!slot) {
		free_prefetch_list(new_pf_list);
		new_pf_list = NULL;
	}
out:
    free_prefetch_list(pf_list);
	pf_list = NULL;
    return new_pf_list;
}

/* process prefetch requests at origin */
int prefetch_at_origin(remote_page_request_t *req)
{
	int from_nid = PCN_KMSG_FROM_NID(req);
	int cnt = 0;
	struct mm_struct *mm;
#if !PREFETCH_REMOTE_REMOTE_IMM
	struct list_head rr_prefetch_list_head; // remote remote prefeth list head
	struct remote_remote_prefetch_req *curr, *next;
#endif
	struct timeval tv_start;
	struct task_struct *tsk;
    struct remote_context *rc;
	struct prefetch_body *list_ptr = (struct prefetch_body*)&req->pf_list;
	if (!req->is_pf_list) return -1;
#ifdef CONFIG_POPCORN_CHECK_SANITY
	BUG_ON(!list_ptr->addr && "is_list but no content");
#endif
#if !PREFETCH_REMOTE_REMOTE_IMM
	INIT_LIST_HEAD(&rr_prefetch_list_head);
#endif
	pf_time_start_at_origin(&tv_start);
	tsk = __get_task_struct(req->remote_pid);
	if (!tsk) BUG();

	mm = get_task_mm(tsk);
	if(!mm) {
		put_task_struct(tsk);
		BUG();
	}

	rc = get_task_remote_with_mm(mm);
	down_read(&mm->mmap_sem);

	while(list_ptr->addr && req->is_pf_list > cnt) {
		int fk;
		pmd_t *pmd;
		pte_t *pte;
		void *paddr;
		int res_size;
		spinlock_t *ptl;
		struct page *page;
		unsigned long flags;
		unsigned long fault_flags;
		bool remote_remote = false;
		struct fault_handle *fh = NULL;
		remote_prefetch_response_t *res;
		bool found = false, leader = false;
        unsigned long addr = list_ptr->addr;
		struct vm_area_struct *vma = find_vma(mm, addr);
#if !PREFETCH_REMOTE_REMOTE_IMM
		bool queue_remote_remote = false;
#endif

		if (TRANSFER_PAGE_WITH_RDMA) {
			res = pcn_kmsg_get(sizeof(remote_page_response_short_t));
		} else {
			res = pcn_kmsg_get(sizeof(*res));
		}

		if (!vma || vma->vm_start > addr || vma->vm_end < addr) {
			PFPRINTK("origin %lx is_vma(%s) or wrong addr range\n",
											addr, vma ? "O" : "X");
			res->result = PREFETCH_FAIL;
			res_size = sizeof(remote_prefetch_fail_t);
			pf_fail_action_record(false, false, false, false, false);
			goto next_nopte;
		}

        pte = __get_pte_at(mm, addr, &pmd, &ptl);

//		if (list_ptr->is_besteffort) {
			spin_lock(ptl);
//		} else {
//			if(!spin_trylock(ptl)) {
//				PFPRINTK("origin unselect %lx pte locked\n", addr);
///				res->result = PREFETCH_FAIL;
//				res_size = sizeof(remote_prefetch_fail_t);
//				goto next;
//			}
//		}

        fk = __fault_hash_key(addr);

//		if (list_ptr->is_besteffort) {
			spin_lock_irqsave(&rc->faults_lock[fk], flags);
//		} else {
//			if(!spin_trylock_irqsave(&rc->faults_lock[fk], flags)) {
//				spin_unlock(ptl);
//				PFPRINTK("origin unselect %lx fh locked\n", addr);
//				res->result = PREFETCH_FAIL;
//				res_size = sizeof(remote_prefetch_fail_t);
//				goto next;
//			}
//		}
        spin_unlock(ptl);

		hlist_for_each_entry(fh, &rc->faults[fk], list) {
			if (fh->addr == addr) {
				found = true;
				break;
			}
		}

		if (found) {
			/* follwer case */
			res->result = PREFETCH_FAIL;
			res_size = sizeof(remote_prefetch_fail_t);
			pf_fail_action_record(false, true, false, true, false);
		} else {
			if (page_is_mine(mm, addr)) {
				/* no conflict and owner */
				leader = true;
				res->result = PREFETCH_SUCCESS;
				res_size = sizeof(remote_prefetch_response_t);
				fh = __alloc_fault_handle(tsk, addr, rc);

				/* remotefault | at remote | read/wirte */ //modulize
				fh->flags = FAULT_HANDLE_REMOTE;
				if (list_ptr->is_write)
					fh->flags |= FAULT_HANDLE_WRITE;
			} else { /* !page_is_mine */
				/* send a remote page request xxx: NOT supported yet, merge with upper */
				/* THIS REMOTE_FETCH MAY FAIL. WHAT IF A FOLLOWER WAITING FOR COALESCING - CONSIDER IT */
				/* prefetch remote remote - not support */
				//res->result = PREFETCH_FAIL;
				//res_size = sizeof(remote_prefetch_fail_t);
				//pf_fail_action_record(false, true, false, false, false);

#if PREFETCH_REMOTE_REMOTE_IMM
				/* prefetch remote remote - support */
				leader = true;
				res_size = sizeof(remote_prefetch_response_t);
				fh = __alloc_fault_handle(tsk, addr, rc);

				/* remotefault | at remote | read/wirte */ //modulize
				fh->flags = FAULT_HANDLE_REMOTE; //remote | r
				if (list_ptr->is_write)
					fh->flags |= FAULT_FLAG_WRITE;
#else
				queue_remote_remote = true;
#endif
			}
		}
		spin_unlock_irqrestore(&rc->faults_lock[fk], flags);

		/* here, we either a leader or follower */
		if (leader) {
			pte_t entry;
			bool grant = false;

			if (test_page_owner(from_nid, mm, addr)) { /* read->write */
				PFPRINTK("%s(): 1 r R->W %lx granted(O)\n", __func__, addr);
				BUG_ON(!list_ptr->is_write && "Read fault req from owner??"); /* can ignore */
				BUG_ON(!rc);
				__claim_local_page(tsk, addr, from_nid, mm);
				grant = true; /* the remote  has had the up-to-date pg */
				res->result |= VM_FAULT_CONTINUE;
				res_size = sizeof(remote_prefetch_fail_t); // fail means: no pg but scucess
			} else { /* requesting node is not owner */
				if (!page_is_mine(mm, addr)) { /* owner = remote remote */
#if PREFETCH_REMOTE_REMOTE_IMM
					res->result = PREFETCH_SUCCESS;
					page = get_normal_page(vma, addr, pte); /* get on-demand */
					BUG_ON(!page); // if this happen, just detour. TODO ask
					remote_remote = true;
					__claim_remote_page(tsk, vma, addr, fh->flags, page, NULL);
#endif
				} else { /* owner = me */
					if (list_ptr->is_write) { /* pg_is_mine | remotefault | write */
						BUG_ON(!rc);
						__claim_local_page(tsk, addr, my_nid, mm); /* revoke others exp origin */
						PFPRINTK("%s(): 2 r X->W | o mine %lx granted(X)\n", __func__, addr);
					} else { /* no revoke */
						PFPRINTK("%s(): 3 r X->R | o mine %lx granted(X)\n", __func__, addr);
					}
				}
			}

			spin_lock(ptl);
			SetPageDistributed(mm, addr);
			set_page_owner(from_nid, mm, addr);
			entry = ptep_clear_flush(vma, addr, pte);

			if (list_ptr->is_write) { /* remotefault | write */
				clear_page_owner(my_nid, mm, addr); /* I'm not owner, requesting is */
				entry = pte_make_invalid(entry);
			} else { /* remotefault | read */
				entry = pte_make_valid(entry); /* For remote-claimed case */
				entry = pte_wrprotect(entry);
				set_page_owner(my_nid, mm, addr); /* shared owners */
			}
			set_pte_at_notify(mm, addr, pte, entry);
			update_mmu_cache(vma, addr, pte);
			spin_unlock(ptl);

			/* copy page to msg */
			if (!grant) {
				if (!remote_remote) {
					page = get_normal_page(vma, addr, pte); /* get on-demand */
					BUG_ON(!page);
				}
				flush_cache_page(vma, addr, page_to_pfn(page));

				/* socket */
				paddr = kmap_atomic(page);
				copy_from_user_page(vma, page, addr, res->page, paddr, PAGE_SIZE);
				kunmap_atomic(paddr);
				/* ib */
				// TODO
			}
			__finish_fault_handling(fh);
		} /* leader done */


#if !PREFETCH_REMOTE_REMOTE_IMM
		if (queue_remote_remote) {
			struct remote_remote_prefetch_req *rrpf_req =
							kmalloc(sizeof(*rrpf_req), GFP_KERNEL);

			res->result = PREFETCH_FAIL;
			res_size = sizeof(remote_prefetch_fail_t);

			rrpf_req->addr = addr;
			rrpf_req->flags = FAULT_HANDLE_REMOTE;
			if (list_ptr->is_write)
				rrpf_req->flags |= FAULT_FLAG_WRITE;
			list_add_tail(&rrpf_req->list, &rr_prefetch_list_head);
			//pf_fail_action_record(false, true, false, false, false);
		}
#endif

next:
		pte_unmap(pte);
next_nopte:
		PFPRINTK("handled pf r%d-#%d:\t%lx (%s)\n",
				PCN_KMSG_FROM_NID(req), cnt, addr,
				res->result & PREFETCH_SUCCESS ? "O" : "X");

		/* msg */
		res->addr = addr;
		res->is_write = list_ptr->is_write;
		res->tgid = rc->remote_tgids[from_nid];
		res->remote_pid = req->remote_pid;
		res->origin_pid = req->origin_pid;
		pcn_kmsg_post(PCN_KMSG_TYPE_REMOTE_PREFETCH_RESPONSE,
						PCN_KMSG_FROM_NID(req), res, res_size);

		list_ptr++;
		cnt++;
		if (cnt >= MAX_PF_REQ) break; /* better way? # in msg */
    }

#if !PREFETCH_REMOTE_REMOTE_IMM
	if (!list_empty(&rr_prefetch_list_head)) {
		list_for_each_entry_safe(curr, next, &rr_prefetch_list_head, list) {
			//goto next_nopte2;
			/* only read pf pf for read*/
			unsigned long addr = curr->addr;
			unsigned long fault_flags = curr->flags;
			//bool is_write = curr->is_write;

			pmd_t *pmd;
			struct fault_handle *fh = NULL;
			bool found = false, leader =false;
			unsigned long flags;
			spinlock_t *ptl;
			int fk;
			pte_t *pte;
			struct vm_area_struct *vma = find_vma(mm, addr);
			if (!vma || vma->vm_start > addr || vma->vm_end < addr) {
				//PFPRINTK("pfpf: origin %lx is_vma(%s) or wrong addr range\n",
				//								addr, vma ? "O" : "X");
				//res->result = PREFETCH_FAIL;
				//res_size = sizeof(remote_prefetch_fail_t);
				//pf_fail_action_record(false, false, false, false, false);
				goto next_nopte2;
			}

			pte = __get_pte_at(mm, addr, &pmd, &ptl);
			if (!pte) {
				BUG(); /* impossible */
				goto next_nopte2;
			}

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
				if (!test_page_owner(from_nid, mm, addr)) { /* read->write */
					if (!page_is_mine(mm, addr)) {
						int err;
						struct page *page = vm_normal_page(vma, addr, *pte);
#ifdef CONFIG_POPCORN_CHECK_SANITY
						BUG_ON(!page);
#endif
						err = __claim_remote_page(current, vma, addr, fault_flags, page, mm);
#ifdef CONFIG_POPCORN_CHECK_SANITY
						BUG_ON(err); // TODO: why 100% success? ask?
#endif
						spin_lock(ptl);
						__make_pte_valid(mm, vma, addr, fault_flags, pte);
						spin_unlock(ptl);
					} else { //TODO: record }
				} else { //TODO: record  }
				__finish_fault_handling(fh);
			}

			pte_unmap(pte);
next_nopte2:
			list_del(&curr->list);
			kfree(curr);
		}
	}
#endif

	up_read(&mm->mmap_sem);
	__put_task_remote(rc);
	mmput(mm);
	put_task_struct(tsk);
	pf_time_end_at_origin(&tv_start);
	return 0;
}

/* prefetch response event handler */
static void process_remote_prefetch_response(struct work_struct *work)
{
	START_KMSG_WORK(remote_prefetch_response_t, res, work);
	pte_t *pte;
	pmd_t *pmd;
	void *paddr;
	spinlock_t *ptl;
	bool out = false;
    struct page *page;
	unsigned long flags;
    struct mm_struct *mm;
	struct timeval tv_start;
	struct fault_handle *fh;
	struct mem_cgroup *memcg;
	struct remote_context *rc;
    struct vm_area_struct *vma;
	int fk, ret = VM_FAULT_RETRY;	/* If prefetch fail, followers retry */
	bool found = false, populated = false;
    struct task_struct *tsk;

    tsk = __get_task_struct(res->tgid);
    if (!tsk) {
		BUG();
        PGPRINTK("%s: no such process %d %d pf_addr %lx\n", __func__,
                res->origin_pid, res->remote_pid, res->addr);
        goto out_fail;
    }

	if (!(res->result & PREFETCH_SUCCESS)) {
		out = true;
		if (!res->is_write)
			pf_action_record(true, false, false, false, true, false);
		else
			pf_action_record(false, false, true, false, true, false); /* xxx */
		goto out;
	}
	mm = get_task_mm(tsk);
	if(page_is_mine(mm, res->addr) && !res->is_write) BUG();
								/* if, concurrency problem */

	down_read(&mm->mmap_sem);
	vma = find_vma(mm, res->addr);
    BUG_ON(!vma || vma->vm_start > res->addr || vma->vm_end < res->addr);

	/* two cases:	1. prefetch not up-to-date
					2. prefetch w/ up-to-date	*/
	/* localfault_at_remote */
	pte = __get_pte_at(mm, res->addr, &pmd, &ptl);
	if (!pte) {
		PGPRINTK("  [%d] No PTE!!\n", tsk->pid);
		BUG();
	}

    /* get a page frame for the vma page if needed */
	if (pte_none(*pte) ||
		!(page = vm_normal_page(vma, res->addr, *pte))) {
		page = alloc_page_vma(GFP_HIGHUSER_MOVABLE, vma, res->addr);
		mem_cgroup_try_charge(page, mm, GFP_KERNEL, &memcg);
		populated = true;
		PFPRINTK("recv:\t\tpopulated(%s) %lx=====================\n",
					populated ? "O" : "X", res->addr);

	}
	get_page(page);

	/* update pte */
	if (res->result & VM_FAULT_CONTINUE) {
		/* bottom-half of localfault_at_remote */
		/**
		 * Page ownership is granted without transferring the page data
		 * since this node already owns the up-to-dated page
		 */
		pte_t entry;
		BUG_ON(populated); /* shouldn't be populated since it was there */
#ifdef CONFIG_POPCORN_CHECK_SANITY
		BUG_ON(!page_is_mine(mm, res->addr));
#endif
		pf_action_record(false, false, false, true, true, true); // WO SUCC 0x0111
		PFPRINTK("recv:\t\t preown & up-dto-date %lx\n", res->addr);

		spin_lock(ptl);
		entry = pte_make_valid(*pte);

		if (fault_for_write(res->is_write ? FAULT_FLAG_WRITE : 0)) {
			entry = pte_mkwrite(entry);
			entry = pte_mkdirty(entry);
		} else {
			entry = pte_wrprotect(entry);
		}
		entry = pte_mkyoung(entry);

		if (ptep_set_access_flags(vma, res->addr, pte, entry, 1)) {
			update_mmu_cache(vma, res->addr, pte);
		}
	} else {
		/* __fetch_page_from_origin() */
		/* load page - not support RDMA now xxx */
		paddr = kmap(page);
		copy_to_user_page(vma, page, res->addr, paddr, res->page, PAGE_SIZE);
		kunmap(page);
		__SetPageUptodate(page);

		/* bottom-half of localfault_at_remote */
		spin_lock(ptl);
		if (populated) {
			do_set_pte(vma, res->addr, page, pte, res->is_write, true);
			mem_cgroup_commit_charge(page, memcg, false);
			lru_cache_add_active_or_unevictable(page, vma);
			pf_action_record(false, false, false, true, true, true); // WO SUCC 0x0111
			PFPRINTK("recv:\t\t not preown %lx\n", res->addr);
		} else {
			unsigned fault_flags = res->is_write ? FAULT_FLAG_WRITE : 0;
			__make_pte_valid(mm, vma, res->addr, fault_flags, pte);
			if (fault_flags)
				pf_action_record(false, false, true, false, true, true); //WX SUCC 0x1x11
			else
				pf_action_record(true, false, false, false, true, true);
			PFPRINTK("recv:\t\t preown but not CONTINUE(up-to-date) %lx\n", res->addr);
		}
	}

	SetPageDistributed(mm, res->addr);
	set_page_owner(my_nid, mm, res->addr);

	pte_unmap_unlock(pte, ptl);
	put_page(page);

	up_read(&mm->mmap_sem);

	ret = 0; /* PREFETCH_SUCCESS */
out:
	PFPRINTK("recv:\t\t>%lx (%s)\n", res->addr,
			res->result & PREFETCH_SUCCESS ? "O" : "X");

	/* The requester is holding the fh as a leader. Release fh. */
	rc = get_task_remote(tsk); /* xxx: a problem? also for put() */
	fk = __fault_hash_key(res->addr);

	/* optimization: conflict with normal execution */
	spin_lock_irqsave(&rc->faults_lock[fk], flags);
	hlist_for_each_entry(fh, &rc->faults[fk], list) {
		if (fh->addr == res->addr) {
			found = true;
			break;
		}
	}
	spin_unlock_irqrestore(&rc->faults_lock[fk], flags);
	put_task_remote(tsk);

#ifdef CONFIG_POPCORN_CHECK_SANITY
	if (!found) {
		if (fh)
			printk("fh->addr %lx ", fh->addr);
		printk("%d %d write(%s) %lx VM_FAULT_CONTINUE(%s) SUCC(%s)\n",
				res->origin_pid, res->remote_pid,
				res->is_write ? "O" : "X", res->addr,
				res->result & VM_FAULT_CONTINUE ? "O" : "X",
				res->result & PREFETCH_SUCCESS ? "O" : "X");
		BUG();
	}
#endif
	/* xxx: counter like trace_pgfault */
	fh->ret = ret; /* To followers - done(0) / retry(VM_FAULT_RETRY) */
	pf_time_end_fh_path(fh, res->result & PREFETCH_SUCCESS);
	__finish_fault_handling(fh);
	if (!out)
		mmput(mm);
	put_task_struct(tsk);
out_fail:
    END_KMSG_WORK(res);
}

/**************************************************************************
 * Routing popcorn messages to workers
 */
DEFINE_KMSG_WQ_HANDLER(remote_page_request);
DEFINE_KMSG_WQ_HANDLER(page_invalidate_request);
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
