/**
 * @file page_server.c
 *
 * Popcorn Linux page server implementation
 * This work was an extension of Marina Sadini MS Thesis, but totally revamped
 * for multi-threaded setup.
 *
 * @author Sang-Hoon Kim, SSRG Virginia Tech 2017
 * @author Ho-Ren (Jack) Chuang, SSRG Virginia Tech 2018
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

#include <asm/tlbflush.h>
#include <asm/cacheflush.h>
#include <asm/mmu_context.h>

#include <popcorn/sync.h>
#include <popcorn/types.h>
#include <popcorn/bundle.h>
#include <popcorn/pcn_kmsg.h>

#include "sync.h"
#include "types.h"
#include "pgtable.h"
#include "wait_station.h"
#include "page_server.h"
#include "fh_action.h"

#include "trace_events.h"

extern bool is_trace_memory_pattern;

/************** working zone *************/
extern void si_read_inc(struct task_struct *tsk, unsigned long addr);
extern void si_writenopg_inc(struct task_struct *tsk, unsigned long addr);
/************** working zone *************/

#define PREFETCH_WRITE 0 /* NOT READY */
#define FIX_DEX_BUG 1

#define PERF_DBG 0
#define BUF_DBG 0
#define PROOF_DBG 0
#define MORE_DEBUG 0 /*2nd proof */

#define MULTI_WRITER_ORIGIN_TEST 0
#define MULTI_WRITER_REMOTE_TEST 0

#define DBG_ADDR_RANGE_ON 0
#define DBG_ADDR_LOW 0x1900000
#define DBG_ADDR_HIGH 0x2000000

#define DBG_ADDR_ON 0
//#define DBG_ADDR 0xad6000
#define DBG_ADDR 0xad7000

#if MORE_DEBUG
//#define SYNCPRINTK2(...) printk(KERN_INFO __VA_ARGS__)
#define SYNCPRINTK2(...) PCNPRINTK_ERR(__VA_ARGS__)
#else
#define SYNCPRINTK2(...)
#endif

#ifdef CONFIG_POPCORN_STAT_PGFAULTS
atomic64_t mm_cnt = ATOMIC64_INIT(0);
atomic64_t mm_time_ns = ATOMIC64_INIT(0);

/* local origin & it has to bring from remote (RW)*/
//atomic64_t ptef_ns = ATOMIC64_INIT(0);
//atomic64_t ptef_cnt = ATOMIC64_INIT(0);
/* local_origin & __claim_remote_page(1)(!pg_mine)(RW) */
atomic64_t clr_ns = ATOMIC64_INIT(0);
atomic64_t clr_cnt = ATOMIC64_INIT(0);

/* local_origin & !pg_mine & !send_revoke_msg & is_page */
atomic64_t fp_ns = ATOMIC64_INIT(0);
atomic64_t fp_cnt = ATOMIC64_INIT(0);

/* local_origin & !pg_mine & !send_revoke_msg & is_page */
atomic64_t fpin_ns = ATOMIC64_INIT(0);
atomic64_t fpin_cnt = ATOMIC64_INIT(0);
atomic64_t fpinh_ns = ATOMIC64_INIT(0);
atomic64_t fpinh_cnt = ATOMIC64_INIT(0);

/* __claim_local_page(pg_mine) & origin */
atomic64_t inv_ns = ATOMIC64_INIT(0);
atomic64_t inv_cnt = ATOMIC64_INIT(0);

/* process_page_invalidate_request */
atomic64_t invh_ns = ATOMIC64_INIT(0);
atomic64_t invh_cnt = ATOMIC64_INIT(0);
/* full rr fault time */
atomic64_t fph_ns = ATOMIC64_INIT(0);
atomic64_t fph_cnt = ATOMIC64_INIT(0);
#endif

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
		ktime_t dt, fault_end = ktime_get();

		dt = ktime_sub(fault_end, current->fault_start);
		trace_pgfault_stat(instruction_pointer(current_pt_regs()),
				current->fault_address, ret,
				current->fault_retry, ktime_to_ns(dt));
		current->fault_address = 0;
        if (ktime_to_ns(dt) < 1000 * MICROSECOND) {
            atomic64_add(ktime_to_ns(dt), &mm_time_ns);
            atomic64_inc(&mm_cnt);
        }
	}
#endif
	return ret;
}

void pf_time_stat(struct seq_file *seq, void *v)
{
#ifdef CONFIG_POPCORN_STAT
	if (seq) {
		seq_printf(seq, "%4s  %10ld.%06ld (s)  %3s %-10ld   %3s %-6ld (us)\n",
					"mm", (atomic64_read(&mm_time_ns) / 1000) / MICROSECOND,
							(atomic64_read(&mm_time_ns) / 1000)  % MICROSECOND,
					"cnt", atomic64_read(&mm_cnt),
					"per", atomic64_read(&mm_cnt) ?
					 atomic64_read(&mm_time_ns)/atomic64_read(&mm_cnt)/1000 : 0);

		//seq_printf(seq, "%4s  %10ld.%06ld (s)  %3s %-10ld   %3s %-6ld (us)\n",
		//			"ptef", (atomic64_read(&ptef_ns) / 1000) / MICROSECOND,
		//					(atomic64_read(&ptef_ns) / 1000)  % MICROSECOND,
		//			"cnt", atomic64_read(&ptef_cnt),
		//			"per", atomic64_read(&ptef_cnt) ?
		//			 atomic64_read(&ptef_ns)/atomic64_read(&ptef_cnt)/1000 : 0);

		seq_printf(seq, "%4s  %10ld.%06ld (s)  %3s %-10ld   %3s %-6ld (us)\n",
					"clr", (atomic64_read(&clr_ns) / 1000) / MICROSECOND,
							(atomic64_read(&clr_ns) / 1000)  % MICROSECOND,
					"cnt", atomic64_read(&clr_cnt),
					"per", atomic64_read(&clr_cnt) ?
					 atomic64_read(&clr_ns)/atomic64_read(&clr_cnt)/1000 : 0);

		/* R: only page (R+!pg_mine) */
		seq_printf(seq, "%4s  %10ld.%06ld (s)  %3s %-10ld   %3s %-6ld (us)\n",
			"fp", (atomic64_read(&fp_ns) / 1000) / MICROSECOND,
					(atomic64_read(&fp_ns) / 1000)  % MICROSECOND,
			"cnt", atomic64_read(&fp_cnt),
			"per", atomic64_read(&fp_cnt) ?
			 atomic64_read(&fp_ns)/atomic64_read(&fp_cnt)/1000 : 0);

		seq_printf(seq, "%4s  %10ld.%06ld (s)  %3s %-10ld   %3s %-6ld (us)\n",
			"fph", (atomic64_read(&fph_ns) / 1000) / MICROSECOND,
					(atomic64_read(&fph_ns) / 1000)  % MICROSECOND,
			"cnt", atomic64_read(&fph_cnt),
			"per", atomic64_read(&fph_cnt) ?
			 atomic64_read(&fph_ns)/atomic64_read(&fph_cnt)/1000 : 0);

		/* W: only inv */
		seq_printf(seq, "%4s  %10ld.%06ld (s)  %3s %-10ld   %3s %-6ld (us)\n",
			"inv", (atomic64_read(&inv_ns) / 1000) / MICROSECOND,
					(atomic64_read(&inv_ns) / 1000)  % MICROSECOND,
			"cnt", atomic64_read(&inv_cnt),
			"per", atomic64_read(&inv_cnt) ?
			 atomic64_read(&inv_ns)/atomic64_read(&inv_cnt)/1000 : 0);

		seq_printf(seq, "%4s  %10ld.%06ld (s)  %3s %-10ld   %3s %-6ld (us)\n",
			"invh", (atomic64_read(&invh_ns) / 1000) / MICROSECOND,
					(atomic64_read(&invh_ns) / 1000)  % MICROSECOND,
			"cnt", atomic64_read(&invh_cnt),
			"per", atomic64_read(&invh_cnt) ?
			 atomic64_read(&invh_ns)/atomic64_read(&invh_cnt)/1000 : 0);

		/* W: page + inv */
		seq_printf(seq, "%4s  %10ld.%06ld (s)  %3s %-10ld   %3s %-6ld (us)\n",
			"fpiv", (atomic64_read(&fpin_ns) / 1000) / MICROSECOND,
					(atomic64_read(&fpin_ns) / 1000)  % MICROSECOND,
			"cnt", atomic64_read(&fpin_cnt),
			"per", atomic64_read(&fpin_cnt) ?
			 atomic64_read(&fpin_ns)/atomic64_read(&fpin_cnt)/1000 : 0);
		seq_printf(seq, "%5s  %9ld.%06ld (s)  %3s %-10ld   %3s %-6ld (us)\n",
			"fpivh", (atomic64_read(&fpinh_ns) / 1000) / MICROSECOND,
					(atomic64_read(&fpinh_ns) / 1000)  % MICROSECOND,
			"cnt", atomic64_read(&fpinh_cnt),
			"per", atomic64_read(&fpinh_cnt) ?
			 atomic64_read(&fpinh_ns)/atomic64_read(&fpinh_cnt)/1000 : 0);
	} else {
        atomic64_set(&mm_cnt, 0);
        atomic64_set(&mm_time_ns, 0);

		//atomic64_set(&ptef_cnt, 0);
		//atomic64_set(&ptef_ns, 0);
		atomic64_set(&clr_cnt, 0);
		atomic64_set(&clr_ns, 0);
		atomic64_set(&fp_ns, 0);
		atomic64_set(&fp_cnt, 0);
		atomic64_set(&fph_ns, 0);
		atomic64_set(&fph_cnt, 0);

		atomic64_set(&inv_cnt, 0);
		atomic64_set(&inv_ns, 0);
		atomic64_set(&invh_cnt, 0);
		atomic64_set(&invh_ns, 0);

		atomic64_set(&fpin_ns, 0);
		atomic64_set(&fpin_cnt, 0);
		atomic64_set(&fpinh_ns, 0);
		atomic64_set(&fpinh_cnt, 0);
	}
#endif
}

static inline int __fault_hash_key(unsigned long address)
{
	return (address >> PAGE_SHIFT) % FAULTS_HASH;
}



/**************************************************************************
 * RCSI
 */
extern bool rcsi_readfault_collect(struct task_struct *tsk, unsigned long addr);
bool __rcsi_readfault_collect(struct task_struct *tsk, unsigned long addr)
{
	return rcsi_readfault_collect(tsk, addr);
}

extern bool is_skipped_region(struct task_struct *tsk);

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

//static inline struct page *__get_page_info_page(struct mm_struct *mm, unsigned long addr, unsigned long *offset)
struct page *__get_page_info_page(struct mm_struct *mm, unsigned long addr, unsigned long *offset)
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
		page = alloc_page(GFP_ATOMIC | __GFP_ZERO);
		BUG_ON(!page);
		set_page_private(page, key);

#ifdef FIX_DEX_BUG
		// After adding WQ_UNBOUND, first create is too fast.
		while (radix_tree_insert(&rc->pages, key, page))
			io_schedule();
#else
		/* Original DeX code */
		BUG_ON(radix_tree_insert(&rc->pages, key, page);
#endif
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
#ifdef CONFIG_POPCORN_CHECK_SANITY
	if (!page) { /* MG-D 2nd pf run */
		PCNPRINTK_ERR("%s:%d set_bit not init yet 0x%lx\n",
									__FILE__, __LINE__, addr);
		BUG();
	}
#endif
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
#ifdef CONFIG_POPCORN_CHECK_SANITY
	if (!page) {
		PCNPRINTK_ERR("%s:%d set_bit not init yet 0x%lx\n",
									__FILE__, __LINE__, addr);
		BUG();
	}
#endif
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
#ifdef FIX_DEX_BUG
	if (!pi) { /* Dex BUG() */
		unsigned long key;
		unsigned long *region;
		struct page *page;
		struct remote_context *rc = mm->remote;
		PCNPRINTK_ERR("%s:%d set_bit not init yet 0x%lx. Auto fixing...\n",
													__FILE__, __LINE__, addr);
		__get_page_info_key(addr, &key, &offset);
		page = __lookup_page_info_page(rc, key);
		region = kmap_atomic(page);
		set_bit(nid, region + offset);
		kunmap_atomic(region);
		return;
	}
#endif
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

void sync_set_page_owner(int nid, struct mm_struct *mm, unsigned long addr)
{
	set_page_owner(nid, mm, addr);
}
void sync_clear_page_owner(int nid, struct mm_struct *mm, unsigned long addr)
{
	clear_page_owner(nid, mm, addr);
}

/* implicitely read only */
/* pte_present: x86 - present, arm - valid */
static inline bool page_is_mine_at_remote_for_ww(pte_t pte)
{
	if (pte_present(pte) && !pte_write(pte))
		return true;
	return false;
}

static inline bool page_is_mine_general(struct task_struct *tsk, struct mm_struct *mm, unsigned long addr, pte_t pte)
{
	if (tsk->at_remote)
		return pte_present(pte);
	else
		return page_is_mine(mm, addr);
}

/**************************************************************************
 * Fault tracking mechanism
 */
enum {
	FAULT_HANDLE_WRITE = 0x01,
	FAULT_HANDLE_INVALIDATE = 0x02,
	FAULT_HANDLE_REMOTE = 0x04,
};

static struct kmem_cache *__fault_handle_cache = NULL;

static struct fault_handle *__alloc_fault_handle(struct task_struct *tsk, unsigned long addr)
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
	fh->rc = get_task_remote(tsk);
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

wait:
	spin_lock_irqsave(&rc->faults_lock[fk], flags);
	hlist_for_each_entry(fh, &rc->faults[fk], list) {
		if (fh->addr == addr) {
			PGPRINTK("  [%d] %s %s ongoing, wait\n", tsk->pid,
				fh->flags & FAULT_HANDLE_REMOTE ? "remote" : "local",
				fh->flags & FAULT_HANDLE_WRITE ? "write" : "read");

			if (my_nid == 0) {
				//BUG_ON(fh->flags & FAULT_HANDLE_INVALIDATE);
				spin_unlock_irqrestore(&rc->faults_lock[fk], flags);
				SYNCPRINTK2("%s: inv confiliction %lx wait *dealed* "
										"(Origin)\n", __func__, addr);
				io_schedule();
				goto wait; // spinnig wait until "not found"
			} else { // remote *dealed // wait // better:
				if (fh->flags & FAULT_HANDLE_INVALIDATE) { /* needed? */
					spin_unlock_irqrestore(&rc->faults_lock[fk], flags);
					SYNCPRINTK2("%s: inv confiliction %lx wait *dealed* "
											"(Remote)\n", __func__, addr);
					io_schedule();
					goto wait; // spinning wait until "not found"
				}
			}
			/* attach to the fh pending list */
			fh->flags |= FAULT_HANDLE_INVALIDATE;
			fh->complete = &complete;
			found = true;
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
	} else {
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

#ifdef CONFIG_POPCORN_CHECK_SANITY
		if (action == FH_ACTION_INVALID) {
			//PCNPRINTK_ERR("%s: no such process %d %d %lx\n", __func__,
			//		req->origin_pid, req->remote_pid, *req->addrs);
			unsigned short i;
			i  = (tsk->at_remote << 5);
			i |= (fh->flags & 0x07) << 2;
			i |= !!(fault_for_write(fault_flags)) << 1;
			i |= !!(fault_flags & FAULT_FLAG_REMOTE) << 0;
			//FAULT_HANDLE_WRITE = 0x01,
			//FAULT_HANDLE_INVALIDATE = 0x02,
			//FAULT_HANDLE_REMOTE = 0x04,
			PCNPRINTK_ERR("%s: r) rm)inv)wri) wri)rm) %lx\n", __func__, addr);
			PCNPRINTK_ERR("%s: %c %c%c%c %c%c\n", __func__,
							i & 0x20?'1':'0',
							i & 0x10?'1':'0',
							i & 0x08?'1':'0',
							i & 0x04?'1':'0',
							i & 0x02?'1':'0',
							i & 0x01?'1':'0');
			if (tsk->at_remote)
				action = FH_ACTION_RETRY;
			else
				BUG();
		}
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

	fh = __alloc_fault_handle(tsk, addr);
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
			complete(fh->complete);
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

pte_t *get_pte_at(struct mm_struct *mm, unsigned long addr, pmd_t **ppmd, spinlock_t **ptlp)
{
	return __get_pte_at(mm, addr, ppmd, ptlp);
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

pte_t *get_pte_at_alloc(struct mm_struct *mm, struct vm_area_struct *vma, unsigned long addr, pmd_t **ppmd, spinlock_t **ptlp)
{
	return __get_pte_at_alloc(mm, vma, addr, ppmd, ptlp);
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
void page_server_panic(bool condition, struct mm_struct *mm, unsigned long address, pte_t *pte, pte_t pte_val, int local_origin)
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
	printk(KERN_ERR "%s: [%d] %lx %p %lx %p %lx LO(%s)\n", __func__,
			current->pid, address, pi, pi_val, pte,
				pte_flags(pte_val), local_origin?"O":"X");
	show_regs(current_pt_regs());
	dump_stack();
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
	put_task_struct(tsk);
	mmput(mm);

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

	//PGPRINTK("INVALIDATE_PAGE local [%d] %lx [%d %d/%d]\n", tsk->pid, addr,
	//SYNCPRINTK("- INVALIDATE_PAGE local [%d] %lx [%d %d/%d]\n", tsk->pid, addr,
	//		req->origin_pid, req->remote_pid, PCN_KMSG_FROM_NID(req));
#if DBG_ADDR_ON
	if (addr == DBG_ADDR) {
		printk("- INVALIDATE_PAGE local [%d] %lx [%d %d/%d]\n", tsk->pid, addr,
				req->origin_pid, req->remote_pid, PCN_KMSG_FROM_NID(req));
	}
#endif
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

/* worker at remote */
/* multi-writer - inv at remotely */
/* may be merge with __do_invalidate_page */
// do_locally_inv_page
/* only return 0, else BUG(); */
int do_locally_inv_page(struct task_struct *tsk, struct mm_struct *mm ,unsigned long addr)
{
    struct vm_area_struct *vma;
    pmd_t *pmd;
    pte_t *pte, entry;
    spinlock_t *ptl;

#ifdef CONFIG_POPCORN_CHECK_SANITY
	BUG_ON(!mm);
#endif

    vma = find_vma(mm, addr);
#ifdef CONFIG_POPCORN_CHECK_SANITY
    if (!vma || vma->vm_start > addr) {
		BUG();
        goto out;
    }
#endif

	/* Fix pte (beautify code) */
	if (my_nid == 0) {
again:
		// [at origin - by mwpf ]
		pte = __get_pte_at_alloc(mm, vma, addr, &pmd, &ptl);
		if (!pte) {
			PGPRINTK("  [%d] No PTE!!\n", tsk->pid);
			BUG();
			return VM_FAULT_OOM;
		}

		if (pte_none(*pte)) {
			int _ret;
			unsigned long fault_flags = FAULT_FLAG_WRITE;
			PGPRINTK("  [%d] handle local fault at origin\n", tsk->pid);
			_ret = handle_pte_fault_origin(mm, vma, addr, pte, pmd, fault_flags);
			/* returned with pte unmapped */
			if (unlikely(_ret & VM_FAULT_RETRY)) {
				/* mmap_sem is released during do_fault */
				BUG(); /* Not support retry for now */
				return VM_FAULT_RETRY;
			}
			if (fault_for_write(fault_flags) && !vma_is_anonymous(vma))
				SetPageCowed(mm, addr);
			goto again;
		}
	} else {
		pte = __get_pte_at(mm, addr, &pmd, &ptl);
		if (!pte) {
			SYNCPRINTK2("%s(): no pte\n", __func__);
			/* ignore for now. Handle it if needed. */
			goto out;
		}

		if (!pte_present(*pte)) {
			if (!pte_none(*pte)) {
				SYNCPRINTK2("%s: pte_none %lx *dealed*\n", __func__, addr);
			} else {
				SYNCPRINTK2("%s: !pte_present %lx *dealed*\n", __func__, addr);
			}
			pte_unmap(pte);
			goto out;
		}

#if DBG_ADDR_ON
		if (addr == DBG_ADDR) {
			PCNPRINTK_ERR("going to be invalidated 0x%lx - present(%s) "
							"pte_none(%s) "
							"tso_region(%s)\n",
							addr, pte_present(*pte)?"O":"X",
							pte_none(*pte)?"O":"X",
							tsk->tso_region?"O":"X");
		}
#endif
	}

	/* If pte exist, must fix permission */
	/* stop the world approach - only conflicts with diff-apply addr. */
	if (my_nid == 0) { /* normal remotefault_at_origin */
		SetPageDistributed(mm, addr);
		set_page_owner(1, mm, addr); // Hardcoded - grant permission

		entry = ptep_clear_flush(vma, addr, pte);

		/* For write */
		clear_page_owner(my_nid, mm, addr); // exclusive permissuon
		entry = pte_make_invalid(entry);

		set_pte_at_notify(mm, addr, pte, entry);
		update_mmu_cache(vma, addr, pte);
	} else { /* inv at remote - regular routin */
		set_page_owner(0, mm, addr); // Hardcoded - grant permission

		clear_page_owner(my_nid, mm, addr);

		BUG_ON(!pte_present(*pte));
		entry = ptep_clear_flush(vma, addr, pte);
		entry = pte_make_invalid(entry);

		set_pte_at_notify(mm, addr, pte, entry);
		update_mmu_cache(vma, addr, pte);
	}

	pte_unmap(pte); /* locks removed */

out:
	return 0;
}


/* no return */
static void __do_remote_invalidate_pages_at_remote(struct task_struct *tsk, page_invalidate_batch_request_t *req)
{
	struct mm_struct *mm = get_task_mm(tsk);
	struct vm_area_struct *vma;
	pmd_t *pmd;
	pte_t *pte, entry;
	spinlock_t *ptl;
	int i;
	int ret = 0; // redundant
	struct fault_handle *fh;
	unsigned long *addrs = req->addrs;

	down_read(&mm->mmap_sem);
	for (i = 0; i < req->tso_wr_cnt; i++) {
		unsigned long addr = addrs[i];
#if DBG_ADDR_ON
		if (addr == DBG_ADDR) {
			SYNCPRINTK("[p] INVALIDATE_PAGE [%d/%lu] [%d] %lx [%d %d/%d]\n", i,
							req->tso_wr_cnt, tsk->pid, addr, req->origin_pid,
									req->remote_pid, PCN_KMSG_FROM_NID(req));
		}
#endif
		BUG_ON(!addr);
		vma = find_vma(mm, addr);
		if (!vma || vma->vm_start > addr) {
			ret = VM_FAULT_SIGBUS; // redundant
			SYNCPRINTK2("%s(): fail to find vma\n", __func__);
			continue;
		}


#if BUF_DBG
		SYNCPRINTK("* INVALIDATE_PAGE [%d/%lu] [%d] %lx [%d %d/%d]\n", i,
						req->tso_wr_cnt, tsk->pid, addr, req->origin_pid,
								req->remote_pid, PCN_KMSG_FROM_NID(req));
#endif
#if DBG_ADDR_ON
		if (addr == DBG_ADDR) {
			SYNCPRINTK("* INVALIDATE_PAGE [%d/%lu] [%d] %lx [%d %d/%d]\n", i,
							req->tso_wr_cnt, tsk->pid, addr, req->origin_pid,
									req->remote_pid, PCN_KMSG_FROM_NID(req));
		}
#endif

		// [[ at remote (regular) ]]
		pte = __get_pte_at(mm, addr, &pmd, &ptl);
		if (!pte) {
			SYNCPRINTK2("%s(): no pte\n", __func__);
			/* ignore for now. should we handle it? */
			continue;
		}

		spin_lock(ptl);

		/* check before fh is better !!  (functionize it) */
		if (!pte_present(*pte)) { /* why not present? */
			if (!pte_none(*pte)) {
				SYNCPRINTK2("%s: pte_none %lx *dealed*\n", __func__, addr);
			} else {
				SYNCPRINTK2("%s: !pte_presenm %lx *dealed*\n", __func__, addr);
			}
			pte_unmap_unlock(pte, ptl);
			continue;
		}

		fh = __start_invalidation(tsk, addr, ptl); // one by one - optimization?

		clear_page_owner(my_nid, mm, addr);


		BUG_ON(!pte_present(*pte));
		entry = ptep_clear_flush(vma, addr, pte);
		entry = pte_make_invalid(entry);

		set_pte_at_notify(mm, addr, pte, entry);
		update_mmu_cache(vma, addr, pte);

		__finish_invalidation(fh);
		pte_unmap_unlock(pte, ptl);
	}

	up_read(&mm->mmap_sem);
	mmput(mm);
}

/* no return */
static int __do_remote_invalidate_pages_at_origin(struct task_struct *tsk, page_invalidate_batch_request_t *req)
{
	struct mm_struct *mm = get_task_mm(tsk);
	struct vm_area_struct *vma;
	pmd_t *pmd;
	pte_t *pte, entry;
	spinlock_t *ptl;
	int i;
	int ret = 0; // redundant
	struct fault_handle *fh;
	unsigned long *addrs = req->addrs;

	down_read(&mm->mmap_sem);
	for (i = 0; i < req->tso_wr_cnt; i++) {
		unsigned long addr = addrs[i];
#if DBG_ADDR_ON
		if (addr == DBG_ADDR) {
			SYNCPRINTK("[p] INVALIDATE_PAGE [%d/%lu] [%d] %lx [%d %d/%d]\n",
						i, req->tso_wr_cnt, tsk->pid, addr, req->origin_pid,
									req->remote_pid, PCN_KMSG_FROM_NID(req));
		}
#endif
		BUG_ON(!addr);
		vma = find_vma(mm, addr);
		if (!vma || vma->vm_start > addr) {
			ret = VM_FAULT_SIGBUS; // redundant
			SYNCPRINTK2("%s(): fail to find vma\n", __func__);
			continue;
		}


#if BUF_DBG
		SYNCPRINTK("* INVALIDATE_PAGE local [%d/%lu] [%d] %lx [%d %d/%d]\n", i,
						req->tso_wr_cnt, tsk->pid, addr, req->origin_pid,
								req->remote_pid, PCN_KMSG_FROM_NID(req));
#endif
#if DBG_ADDR_ON
		if (addr == DBG_ADDR) {
			SYNCPRINTK("* INVALIDATE_PAGE local [%d/%lu] [%d] %lx [%d %d/%d]\n", i,
							req->tso_wr_cnt, tsk->pid, addr, req->origin_pid,
									req->remote_pid, PCN_KMSG_FROM_NID(req));
		}
#endif

again:
		// [at origin]
		pte = __get_pte_at_alloc(mm, vma, addr, &pmd, &ptl);
		if (!pte) {
			PGPRINTK("  [%d] No PTE!!\n", tsk->pid);
			BUG();
			return VM_FAULT_OOM;
		}

		spin_lock(ptl);
		if (pte_none(*pte)) {
			int ret;
			unsigned long fault_flags = FAULT_FLAG_WRITE;
			spin_unlock(ptl);
			PGPRINTK("  [%d] handle local fault at origin\n", tsk->pid);
			ret = handle_pte_fault_origin(mm, vma, addr, pte, pmd, fault_flags);
			/* returned with pte unmapped */
			if (ret & VM_FAULT_RETRY) {
				/* mmap_sem is released during do_fault */
				BUG(); /* Not support retry for now */
				return VM_FAULT_RETRY;
			}
			if (fault_for_write(fault_flags) && !vma_is_anonymous(vma))
				SetPageCowed(mm, addr);
			goto again;
		}

		/* check before fh is better !!  (functionize it) */
		if (!pte_present(*pte)) { /* why not present? */
			if (!pte_none(*pte)) {
				SYNCPRINTK2("%s: pte_none %lx *dealed*\n", __func__, addr);
			} else {
				SYNCPRINTK2("%s: !pte_presenm %lx *dealed*\n", __func__, addr);
			}
			pte_unmap_unlock(pte, ptl);
			continue;
		}

		fh = __start_invalidation(tsk, addr, ptl); // one by one (optimize it)

#if 1
		SetPageDistributed(mm, addr);
		set_page_owner(1, mm, addr); // Hard code // grant permission

		entry = ptep_clear_flush(vma, addr, pte);

		clear_page_owner(my_nid, mm, addr); // exclusive permissuon
		entry = pte_make_invalid(entry);

		set_pte_at_notify(mm, addr, pte, entry);
		update_mmu_cache(vma, addr, pte);
		/*  normal remotefault_at_origin */
#endif

		__finish_invalidation(fh);
		pte_unmap_unlock(pte, ptl);
	}

	up_read(&mm->mmap_sem);
	mmput(mm);
	return 0;
}


static void process_page_invalidate_request(struct work_struct *work)
{
#ifdef CONFIG_POPCORN_STAT_PGFAULTS
	ktime_t dt, invh_end, invh_start = ktime_get();
#endif
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

#ifdef CONFIG_POPCORN_STAT_PGFAULTS
	if (is_trace_memory_pattern) {
		invh_end = ktime_get();
		dt = ktime_sub(invh_end, invh_start);
		atomic64_add(ktime_to_ns(dt), &invh_ns);
		atomic64_inc(&invh_cnt);
	}
#endif

out_free:
	END_KMSG_WORK(req);
}


static void process_page_invalidate_batch_request(struct work_struct *work)
{
	START_KMSG_WORK(page_invalidate_batch_request_t, req, work);
	page_invalidate_response_t *res;
	struct task_struct *tsk;

	res = pcn_kmsg_get(sizeof(*res));
	res->origin_pid = req->origin_pid;
	res->origin_ws = req->origin_ws;
	res->remote_pid = req->remote_pid;

	if (my_nid) { /* remote: default popcorn case */
		/* Only home issues invalidate requests. Hence, I am a remote */
		SYNCPRINTK("%s: [%d] [o %d r (%d)] [0]%lx /%lu\n", __func__, current->pid,
				req->origin_pid, req->remote_pid, *req->addrs, req->tso_wr_cnt);
		tsk = __get_task_struct(req->remote_pid);
	} else { /* origin: */
		SYNCPRINTK("%s: [%d] [o (%d) r %d] [0]%lx /%lu\n", __func__, current->pid,
				req->origin_pid, req->remote_pid, *req->addrs, req->tso_wr_cnt);
		//tsk = __get_task_struct(req->origin_pid); // (x)
		tsk = __get_task_struct(req->remote_pid); // (o)
	}
	if (!tsk) {
		PCNPRINTK_ERR("%s: no such process %d %d %lx\n", __func__,
				req->origin_pid, req->remote_pid, *req->addrs);
		pcn_kmsg_put(res);
		BUG(); // shouldn't happen - cannot find address space
		goto out_free;
	}

	if (my_nid == 0)
		__do_remote_invalidate_pages_at_origin(tsk, req);
	else
		__do_remote_invalidate_pages_at_remote(tsk, req);

	SYNCPRINTK(">>[%d] ->[%d/%d] /%lu\n", req->remote_pid, res->origin_pid,
			PCN_KMSG_FROM_NID(req), req->tso_wr_cnt);
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


void __revoke_page_ownership(struct task_struct *tsk, int nid, pid_t pid, unsigned long addr, int ws_id)
{
	page_invalidate_request_t *req = pcn_kmsg_get(sizeof(*req));

	req->addr = addr;
	req->origin_pid = tsk->pid;
	req->origin_ws = ws_id;
	req->remote_pid = pid;

	PGPRINTK("  [%d] send revoke %lx [%d/%d]\n", tsk->pid, addr, pid, nid);
	pcn_kmsg_post(PCN_KMSG_TYPE_PAGE_INVALIDATE_REQUEST, nid, req, sizeof(*req));
}

void remote_revoke_page_ownerships(struct task_struct *tsk, int nid, pid_t pid, unsigned long *addr, unsigned long tso_wr_cnt)
{
	int  i;
	page_invalidate_batch_request_t *req = pcn_kmsg_get(sizeof(*req));
	struct wait_station *ws = get_wait_station(current);
	BUG_ON(!tso_wr_cnt);

	req->origin_pid = tsk->pid;
	req->remote_pid = pid;
	req->origin_ws = ws->id;
	req->tso_wr_cnt = tso_wr_cnt;
	memcpy(&req->addrs, addr, sizeof(*req->addrs) * tso_wr_cnt);

	if(my_nid == 0) {
		for (i = 0; i < req->tso_wr_cnt; i++) {
			int peers;
			unsigned long offset, *pi, addr = req->addrs[i];
			struct page *pip = __get_page_info_page(current->mm, addr, &offset);

			BUG_ON(!addr);
			if (!pip) BUG(); //return; /* skip claiming non-distributed page */
			pi = (unsigned long *)kmap(pip) + offset;
			peers = bitmap_weight(pi, MAX_POPCORN_NODES);
			if (!peers) {
				kunmap(pip);
				SYNCPRINTK2("%s: [%d] NOT PERFECT WORKAROUND [%d/%lu] %lx skip at "
							"here. HOPEFULLY another node will also skip it "
							"(Ithinkso)\n", __func__, current->pid, i,
												req->tso_wr_cnt, addr);
				continue;
			}
			clear_bit(nid, pi);
			kunmap(pip);
		}
	}
	PGPRINTK("  [%d] revoke a batch addrs[0]%lx [%d/%d]\n",
								tsk->pid, *addr, pid, nid);

	pcn_kmsg_post(PCN_KMSG_TYPE_PAGE_INVALIDATE_BATCH_REQUEST,
										nid, req, sizeof(*req));
	wait_at_station(ws);
}

/**************************************************************************
 * Voluntarily release page ownership
 */
int process_madvise_release_from_remote(int from_nid, unsigned long start, unsigned long end)
{
	struct mm_struct *mm;
	unsigned long addr;
	int nr_pages = 0;

	mm = get_task_mm(current);
	for (addr = start; addr < end; addr += PAGE_SIZE) {
		pmd_t *pmd;
		pte_t *pte;
		spinlock_t *ptl;
		pte = __get_pte_at(mm, addr, &pmd, &ptl);
		if (!pte) continue;
		spin_lock(ptl);
		if (!pte_none(*pte)) {
			clear_page_owner(from_nid, mm, addr);
			nr_pages++;
		}
		pte_unmap_unlock(pte, ptl);
	}
	mmput(mm);
	VSPRINTK("  [%d] %d %d / %ld %lx-%lx\n", current->pid, from_nid,
			nr_pages, (end - start) / PAGE_SIZE, start, end);
	return 0;
}

int page_server_release_page_ownership(struct vm_area_struct *vma, unsigned long addr)
{
	struct mm_struct *mm = vma->vm_mm;
	pmd_t *pmd;
	pte_t *pte;
	pte_t pte_val;
	spinlock_t *ptl;

	pte = __get_pte_at(mm, addr, &pmd, &ptl);
	if (!pte) return 0;

	spin_lock(ptl);
	if (pte_none(*pte) || !pte_present(*pte)) {
		pte_unmap_unlock(pte, ptl);
		return 0;
	}

	clear_page_owner(my_nid, mm, addr);
	pte_val = ptep_clear_flush(vma, addr, pte);
	pte_val = pte_make_invalid(pte_val);

	set_pte_at_notify(mm, addr, pte, pte_val);
	update_mmu_cache(vma, addr, pte);
	pte_unmap_unlock(pte, ptl);
	return 1;
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

static int __request_remote_page(struct task_struct *tsk, int from_nid, pid_t from_pid, unsigned long addr, unsigned long fault_flags, int ws_id, struct pcn_kmsg_rdma_handle **rh)
{
	remote_page_request_t *req;

	*rh = NULL;

	req = pcn_kmsg_get(sizeof(*req));
	req->addr = addr;
	req->fault_flags = fault_flags;

	req->origin_pid = tsk->pid;
	req->origin_ws = ws_id;

	req->remote_pid = from_pid;
	req->instr_addr = instruction_pointer(current_pt_regs());

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
	return 0;
}


static remote_page_response_t *__fetch_page_from_origin(struct task_struct *tsk, struct vm_area_struct *vma, unsigned long addr, unsigned long fault_flags, struct page *page)
{
	remote_page_response_t *rp;
	struct wait_station *ws = get_wait_station(tsk);
	struct pcn_kmsg_rdma_handle *rh;

	__request_remote_page(tsk, tsk->origin_nid, tsk->origin_pid,
			addr, fault_flags, ws->id, &rh);

#if GOD_VIEW
	/* Use waiting time to collect read-fault data */
	if (fault_for_read(fault_flags)) {
		__rcsi_readfault_collect(tsk, addr);
		si_read_inc(tsk, addr);
	} else { // fault_for_write
		si_writenopg_inc(tsk, addr);
	}
#endif

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

static int __claim_remote_page(struct task_struct *tsk, struct mm_struct *mm, struct vm_area_struct *vma, unsigned long addr, unsigned long fault_flags, struct page *page, int local_origin)
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
#ifdef CONFIG_POPCORN_STAT_PGFAULTS
	int page_trans = 0;
	ktime_t fp_start;
	if (local_origin) /* aka !pg_mine */
		fp_start = ktime_get();
#endif
	BUG_ON(!pip);

	peers = bitmap_weight(pi, MAX_POPCORN_NODES);

	if (test_bit(my_nid, pi)) {
		peers--;
	}
#ifdef CONFIG_POPCORN_CHECK_SANITY
	if (!current->tso_region)
		page_server_panic(peers == 0, mm, addr, NULL, __pte(0), local_origin);
#endif
	from = random % peers;

	if (fault_for_read(fault_flags)) {
		peers = 1;
	}
	ws = get_wait_station_multiple(tsk, peers);

	for_each_set_bit(nid, pi, MAX_POPCORN_NODES) {
		pid_t pid = rc->remote_tgids[nid];
		if (nid == my_nid) continue;
		if (from-- == 0) {
			from_nid = nid;
			__request_remote_page(tsk, nid, pid, addr, fault_flags, ws->id, &rh);
		} else {
			if (fault_for_write(fault_flags)) {
				clear_bit(nid, pi);
				__revoke_page_ownership(tsk, nid, pid, addr, ws->id);
#ifdef CONFIG_POPCORN_STAT_PGFAULTS
				BUG_ON("Two nodes shouldn't send stand along inv");
#endif
			}
		}
		if (--peers == 0) break;
	}

#if GOD_VIEW
	/* Use waiting time to collect read-fault data */
	/* !page_is_mine */
	if (local_origin) {
		if (fault_for_read(fault_flags)) {
			__rcsi_readfault_collect(tsk, addr);
			si_read_inc(tsk, addr);
		} else {
			si_writenopg_inc(tsk, addr);
		}
	}
#endif
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
#ifdef CONFIG_POPCORN_STAT_PGFAULTS
		page_trans = 1;
#endif
	}
	pcn_kmsg_done(rp);

	if (rh) pcn_kmsg_unpin_rdma_buffer(rh);
	__put_task_remote(rc);
	kunmap(pip);

#ifdef CONFIG_POPCORN_STAT_PGFAULTS
	 if (is_trace_memory_pattern) {
		if (my_nid == 0 && local_origin) {
			if (fault_for_write(fault_flags)) { /* page + inv */
				ktime_t dt, fp_end = ktime_get();
				dt = ktime_sub(fp_end, fp_start);
				atomic64_add(ktime_to_ns(dt), &fpin_ns);
				atomic64_inc(&fpin_cnt);
			} else { /* page + !inv  */
					ktime_t dt, fp_end = ktime_get();
					dt = ktime_sub(fp_end, fp_start);
					atomic64_add(ktime_to_ns(dt), &fp_ns);
					atomic64_inc(&fp_cnt);
			}
			if (!page_trans)
				BUG_ON("!pg_mine must transfer page");
		}
	}
#endif
	return 0;
}


static void __claim_local_page(struct task_struct *tsk, unsigned long addr, int except_nid)
{
	struct mm_struct *mm = tsk->mm;
	unsigned long offset;
	struct page *pip = __get_page_info_page(mm, addr, &offset);
	unsigned long *pi;
	int peers;
#ifdef CONFIG_POPCORN_STAT_PGFAULTS
	int is_inv = 0;
	ktime_t dt, inv_end, inv_start;
#endif

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

#ifdef CONFIG_POPCORN_STAT_PGFAULTS
	inv_start = ktime_get();
#endif

	if (peers > 0) {
		int nid;
		struct remote_context *rc = get_task_remote(tsk);
		struct wait_station *ws = get_wait_station_multiple(tsk, peers);

		for_each_set_bit(nid, pi, MAX_POPCORN_NODES) {
			pid_t pid = rc->remote_tgids[nid];
			if (nid == except_nid || nid == my_nid) continue;

			clear_bit(nid, pi);
			__revoke_page_ownership(tsk, nid, pid, addr, ws->id);
#ifdef CONFIG_POPCORN_STAT_PGFAULTS
			is_inv = 1;
#endif
		}
		put_task_remote(tsk);

		wait_at_station(ws);
	}

#ifdef CONFIG_POPCORN_STAT_PGFAULTS
	if (is_trace_memory_pattern) {
		if (is_inv) { /* at origin */
			inv_end = ktime_get();
			dt = ktime_sub(inv_end, inv_start);
			atomic64_add(ktime_to_ns(dt), &inv_ns);
			atomic64_inc(&inv_cnt);
		}
	}
#endif

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

/* used when page was not mine */
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
 * Prefetch helper fiunctions
 */
/* The pf requester is holding the fh as a leader. Release fh. */
static void __pf_finish(int result, int ret, struct fault_handle *fh)
{
#ifdef CONFIG_POPCORN_CHECK_SANITY
    BUG_ON(!fh);
#endif
    fh->ret = ret; /* To followers - done(0) / retry(VM_FAULT_RETRY) */
    __finish_fault_handling(fh);
}

// return:  fh (select) : NULL (!select)
struct fault_handle *select_prefetch_page(unsigned long addr)
{
	bool found = false;
	struct fault_handle *select = NULL;
	int fk;
    pte_t *pte;
    pmd_t *pmd;
    spinlock_t *ptl;
    unsigned long flags;
    struct fault_handle *fh;
	struct mm_struct *mm = current->mm;
    struct remote_context *rc = mm->remote;

#ifdef CONFIG_POPCORN_CHECK_SANITY
    struct vm_area_struct *vma = find_vma(mm, addr);
    if (!vma || vma->vm_start > addr) {
		PCNPRINTK_ERR("CANNOT select vma %p mm %p addr 0x%lx\n", vma, mm, addr);
		rc->wrong_hist_no_vma++;
		goto out;
	}
#endif

    pte = __get_pte_at(mm, addr, &pmd, &ptl);
    if (!pte) {
		/* never touched - if happens, handle it */
        goto out;
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

	if (!found) { /* bz - concurrency from remote */
        if (!page_is_mine_general(current, mm, addr, *pte)) {
			/* Assumptions: rc will not disappear. Must be read */
			select = __alloc_fault_handle(current, addr);
		} else {
			/* god view history is wrong - I already own - skip */
			rc->wrong_hist++;
		}
	} else {
		// record counter here, if always 0 for all the cases
		// fh checking may be simply skipped
		// need_lock_for_pf_select_cnt++; (not thread safe)
		// once we can remove fh, we can also remove the pf_map as well !!
	}
	spin_unlock_irqrestore(&rc->faults_lock[fk], flags);
	pte_unmap(pte);

out:
	return select;
}

struct fault_handle *select_prefetch_page2(unsigned long addr)
{
	struct fault_handle *select = NULL;
    pte_t *pte;
    pmd_t *pmd;
    spinlock_t *ptl;
	struct mm_struct *mm = current->mm;
    struct remote_context *rc = mm->remote;
	bool owned;

#ifdef CONFIG_POPCORN_CHECK_SANITY
    struct vm_area_struct *vma = find_vma(mm, addr);
    if (!vma || vma->vm_start > addr) {
		PCNPRINTK_ERR("CANNOT select vma %p mm %p addr 0x%lx\n", vma, mm, addr);
		rc->wrong_hist_no_vma++;
		goto out;
	}
#endif

    pte = __get_pte_at(mm, addr, &pmd, &ptl);
    if (!pte) {
		/* never touched - if happens, handle it */
        goto out;
	}

	/* do I owned the page? */
	owned = page_is_mine_general(current, mm, addr, *pte);
	if (!owned)
		select = (void*)0x12345678;
	else
		rc->wrong_hist++;

	pte_unmap(pte);

out:
	return select;
}

static void __prefetch_each(struct mm_struct *mm,
				struct task_struct *tsk,
				struct remote_context *rc,
				struct prefetch_list_body *list_curr,
				int pf_num, int *res_size, int succ_pf_num,
				int from_nid, remote_prefetch_response_t *res,
				struct list_head *preprefetch_list_head, char *pf_addr,
				int pf_list_size)
{
    int fk;
    pmd_t *pmd;
    void *paddr;
    spinlock_t *ptl;
    pte_t *pte, entry;
    struct page *page;
    unsigned long flags;
    bool is_write = false;
	struct vm_area_struct *vma;
    struct fault_handle *fh = NULL;
    unsigned long addr = list_curr->addr;
	bool null_pte = false, pg_mine = true;
    bool found = false, leader = false, grant = false, is_conflict = false;

    res->pf_results[pf_num].addr = addr;
	res->pf_results[pf_num].result = PREFETCH_FAIL;

#ifdef CONFIG_POPCORN_CHECK_SANITY
	BUG_ON(!addr);
#endif

    vma = find_vma(mm, list_curr->addr);
    if (!vma || vma->vm_start > list_curr->addr) {
        PFPRINTK("rreq addr 0x%lx is_vma(%s) or wrong addr range\n",
                                list_curr->addr, vma ? "O" : "X");
        BUG();
    }

    pte = __get_pte_at(mm, addr, &pmd, &ptl);
	if (!pte) { /* Two node info different - remote doesn't always have pte */
		BUG_ON(!tsk->at_remote);
		null_pte = true;
		goto out;
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

	if (found) {
		is_conflict = true;
		res->pf_results[pf_num].result = PREFETCH_FAIL;
    } else {
        if (page_is_mine_general(tsk, mm, addr, *pte)) {
            leader = true;
			res->pf_results[pf_num].result = PREFETCH_SUCCESS;
#if !SI_IDEAL
            *res_size += PAGE_SIZE;
#endif
            fh = __alloc_fault_handle(tsk, addr);
			fh->ret = VM_FAULT_RETRY; /* Bug fixed */

            /* remotefault | at remote/origin | read/wirte */
            fh->flags = FAULT_HANDLE_REMOTE; /* not a local fault */
        } else {
			pg_mine = false;
			res->pf_results[pf_num].result = PREFETCH_FAIL;
        }
    }
    spin_unlock_irqrestore(&rc->faults_lock[fk], flags);

	if (!leader) {
        goto next;
	}

	// leader
	// remotefault_at_origin code
	spin_lock(ptl);
    SetPageDistributed(mm, addr);
	if (!tsk->at_remote)
		set_page_owner(from_nid, mm, addr); /* No matter pf with r/w permission!! */

	entry = ptep_clear_flush(vma, addr, pte); /* WARN: remote doesn't like it */

	if (likely(!is_write)) { /* remotefault | read */
		if (!tsk->at_remote)
			entry = pte_make_valid(entry); /* For remote-claimed case */
        entry = pte_wrprotect(entry);
		if (!tsk->at_remote)
			set_page_owner(my_nid, mm, addr); /* Shared owners */
    } else {
		BUG();
    }
    set_pte_at_notify(mm, addr, pte, entry);
    update_mmu_cache(vma, addr, pte);
    spin_unlock(ptl);

	/* Copy page to msg */
    if (!grant) {
		page = get_normal_page(vma, addr, pte);
		BUG_ON(!page);
        flush_cache_page(vma, addr, page_to_pfn(page));

#if !SI_IDEAL
        paddr = kmap_atomic(page);
#if PREFETCH_WRITE		// || PREFETCH_WPOLL
        copy_from_user_page(vma, page, addr,
			pf_addr + (succ_pf_num * PAGE_SIZE), paddr, PAGE_SIZE);
#else
        copy_from_user_page(vma, page, addr,
            &res->page[succ_pf_num][0], paddr, PAGE_SIZE);
#endif
        kunmap_atomic(paddr);
#endif
    }

    __finish_fault_handling(fh);

next:
    pte_unmap(pte);

out:
	;
}

// general
void handle_prefetch(remote_prefetch_request_t *req)
{
    struct mm_struct *mm;
    struct remote_context *rc;
    remote_prefetch_response_t *res;
	int from_nid = PCN_KMSG_FROM_NID(req);
	struct task_struct *tsk = __get_task_struct(req->remote_pid);
    struct list_head preprefetch_list_head;
    int cnt = 0, res_size, succ_pf_num = 0;
    char *pf_addr = NULL;
	int remote_pid = req->remote_pid;
	int origin_pid = req->origin_pid;
	int pf_list_size = req->pf_list_size;
	int pf_req_id = req->pf_req_id;
	struct prefetch_list_body *list_curr = req->pf_reqs;
#if PREFETCH_WRITE
    struct prefetch_list_body *list_first = list_curr;
    struct pcn_kmsg_rdma_handle *pf_handle = NULL;
#endif
#ifdef CONFIG_POPCORN_CHECK_SANITY
    BUG_ON(!pf_list_size);
    BUG_ON(!list_curr->addr && "is_list but no content");
#endif

	BUG_ON(!tsk);
    mm = tsk->mm;
	BUG_ON(!mm);
	rc = mm->remote;
	BUG_ON(!rc);

    if (TRANSFER_PAGE_WITH_RDMA) {
#if PREFETCH_WRITE
        pf_handle = pcn_kmsg_pin_rdma_buffer(NULL, PAGE_SIZE * (MAX_PF_REQ));
        pf_addr = pf_handle->addr;
        res = pcn_kmsg_get(sizeof(remote_prefetch_response_short_t));
#else
        res = pcn_kmsg_get(sizeof(*res));
#endif
    } else {
        res = pcn_kmsg_get(sizeof(*res));
    }

	res_size = sizeof(remote_prefetch_response_short_t);
    res->remote_pid = remote_pid;
    res->origin_pid = origin_pid;
    res->pf_req_id = pf_req_id;
	res->pf_list_size = pf_list_size;

	res->god_omp_hash = req->god_omp_hash;

	down_read(&mm->mmap_sem);
	while (list_curr->addr && cnt < pf_list_size) {
        __prefetch_each(mm, tsk, rc, list_curr, cnt,
                                &res_size, succ_pf_num, from_nid,
							res, &preprefetch_list_head, pf_addr, pf_list_size);
        if (res->pf_results[cnt].result & PREFETCH_SUCCESS)
            succ_pf_num++;

        cnt++;
        list_curr++;
    }

    if (TRANSFER_PAGE_WITH_RDMA) {
#if PREFETCH_WRITE
		pcn_kmsg_rdma_write(from_nid, list_first->rdma_addr, pf_handle->dma_addr,
							NULL, succ_pf_num * PAGE_SIZE, list_first->rdma_key);
        pcn_kmsg_unpin_rdma_buffer(pf_handle);

        pcn_kmsg_post(PCN_KMSG_TYPE_REMOTE_PREFETCH_RESPONSE,
						from_nid, res,
						sizeof(remote_prefetch_response_short_t));
#else
        pcn_kmsg_post(PCN_KMSG_TYPE_REMOTE_PREFETCH_RESPONSE,
									from_nid, res, res_size);
#endif
    } else {
        pcn_kmsg_post(PCN_KMSG_TYPE_REMOTE_PREFETCH_RESPONSE,
									from_nid, res, res_size);
    }
#if 1
	PFPRINTK("\t\t%d<-sent back hash 0x%lx #%d/-[succ%d/%d] 0x%lx size %d\n",
						from_nid, req->god_omp_hash, res->pf_req_id,
						succ_pf_num, pf_list_size,
						req->pf_reqs[pf_list_size - 1].addr, res_size);
#endif
	up_read(&mm->mmap_sem);
    put_task_struct(tsk);
}
extern unsigned long real_recv_pf_succ;
extern unsigned long real_recv_pf_fail;
// IMPORTANT - int/process context
void __fixup_page(remote_prefetch_response_t *res, int pf_num, int *succ_pf_num, struct task_struct *tsk, struct fault_handle *fh)
{
	pte_t *pte;
	pmd_t *pmd;
	void *paddr;
	spinlock_t *ptl;
	struct page *page;
	struct mem_cgroup *memcg;
	int ret = VM_FAULT_RETRY;
	bool populated = false;
	int result = res->pf_results[pf_num].result;
	unsigned long addr = res->pf_results[pf_num].addr;
#if PREFETCH_WRITE
	char *res_page = pf_handle->addr + (*succ_pf_num * PAGE_SIZE);
#else
	char *res_page = &res->page[*succ_pf_num][0];
#endif
	bool is_write = false;
	unsigned fault_flags = is_write ? FAULT_FLAG_WRITE : 0;
	struct mm_struct *mm = tsk->mm;
	struct vm_area_struct *vma = find_vma(mm, addr);
#ifdef CONFIG_POPCORN_CHECK_SANITY
	BUG_ON(!addr);
#endif

	BUG_ON(!vma || vma->vm_start > addr);
	if (!(result & PREFETCH_SUCCESS)) {
		real_recv_pf_fail++; /* log only */
		goto next;
	}
	real_recv_pf_succ++; /* log only */
	(*succ_pf_num) += 1;

	pte = __get_pte_at(mm, addr, &pmd, &ptl);
	if (!pte)
		BUG_ON(printk("  [%d] No PTE!!\n", tsk->pid));

#ifdef CONFIG_POPCORN_CHECK_SANITY
	BUG_ON(page_is_mine_general(tsk, mm, addr, *pte));
#endif

	/* get a page frame for the vma page if needed */
	/* optimization: at lease move to !(result & VM_FAULT_CONTINUE) */
	/* optimization: then see if we can hide this. Perf it first!! */
	/* optimization: can I do this while waitng?  */
	if (pte_none(*pte) || !(page = vm_normal_page(vma, addr, *pte))) {
		BUG_ON(!tsk->at_remote); /* origin vm_normal_page must succeed */
		page = alloc_page_vma(GFP_HIGHUSER_MOVABLE, vma, addr);
		BUG_ON(mem_cgroup_try_charge(page, mm, GFP_KERNEL, &memcg));
		populated = true;
		PFPRINTK("recv:\t\tpopulated(%s) %lx=====================\n",
		          populated ? "O" : "X", addr);
	}
	get_page(page); /* origin doesn't need to get_page */

#if !SI_IDEAL
	/* must be READ */
	paddr = kmap(page);
	copy_to_user_page(vma, page, addr, paddr, res_page, PAGE_SIZE);
	kunmap(page);
#endif
	flush_dcache_page(page);
	__SetPageUptodate(page);

	spin_lock(ptl);
	if (!tsk->at_remote) {
		__make_pte_valid(mm, vma, addr, fault_flags, pte);
	} else  { /* bottom-half of localfault_at_remote */
		if (populated) {
			do_set_pte(vma, addr, page, pte, is_write, true);
			mem_cgroup_commit_charge(page, memcg, false);
			lru_cache_add_active_or_unevictable(page, vma);

			SetPageDistributed(mm, addr);
			set_page_owner(my_nid, mm, addr);
		} else {
			__make_pte_valid(mm, vma, addr, fault_flags, pte);
		}
	}

	pte_unmap_unlock(pte, ptl);
	put_page(page); /* origin doesn't need to put_page */

	ret = 0; /* PREFETCH_SUCCESS */

next:
	//printk("\t\t\tfixup end 0x%lx\n", addr);
	__pf_finish(result, ret, fh);
}

void __fixup_page2(remote_prefetch_response_t *res, int pf_num, int *succ_pf_num, struct task_struct *tsk)
{
	pte_t *pte;
	pmd_t *pmd;
	void *paddr;
	spinlock_t *ptl;
	struct page *page;
	struct mem_cgroup *memcg;
	int ret = VM_FAULT_RETRY;
	bool populated = false;
	int result = res->pf_results[pf_num].result;
	unsigned long addr = res->pf_results[pf_num].addr;
#if PREFETCH_WRITE
	char *res_page = pf_handle->addr + (*succ_pf_num * PAGE_SIZE);
#else
	char *res_page = &res->page[*succ_pf_num][0];
#endif
	bool is_write = false;
	unsigned fault_flags = is_write ? FAULT_FLAG_WRITE : 0;
	struct mm_struct *mm = tsk->mm;
	struct vm_area_struct *vma = find_vma(mm, addr);
#ifdef CONFIG_POPCORN_CHECK_SANITY
	BUG_ON(!addr);
#endif

	//printk("\t\t\tfixup start - vma %p mm %p addr 0x%lx\n", vma, mm, addr);
	BUG_ON(!vma || vma->vm_start > addr);
	if (!(result & PREFETCH_SUCCESS)) {
		real_recv_pf_fail++; /* log only */
		goto next;
	}
	real_recv_pf_succ++; /* log only */
	(*succ_pf_num) += 1;

	pte = __get_pte_at(mm, addr, &pmd, &ptl);
	if (!pte)
		BUG_ON(printk("  [%d] No PTE!!\n", tsk->pid));

#ifdef CONFIG_POPCORN_CHECK_SANITY
	BUG_ON(page_is_mine_general(tsk, mm, addr, *pte));
#endif

	/* get a page frame for the vma page if needed */
	/* optimization: at lease move to !(result & VM_FAULT_CONTINUE) */
	/* optimization: then see if we can hide this. Perf it first!! */
	/* optimization: can I do this while waitng?  */
	if (pte_none(*pte) || !(page = vm_normal_page(vma, addr, *pte))) {
		BUG_ON(!tsk->at_remote); /* origin vm_normal_page must succeed */
		page = alloc_page_vma(GFP_HIGHUSER_MOVABLE, vma, addr);
		BUG_ON(mem_cgroup_try_charge(page, mm, GFP_KERNEL, &memcg));
		populated = true;
		PFPRINTK("recv:\t\tpopulated(%s) %lx=====================\n",
		          populated ? "O" : "X", addr);
	}
	get_page(page); /* origin doesn't need to get_page */

#if !SI_IDEAL
	/* must be READ */
	paddr = kmap(page);
	copy_to_user_page(vma, page, addr, paddr, res_page, PAGE_SIZE);
	kunmap(page);
#endif
	flush_dcache_page(page);
	__SetPageUptodate(page);

	if (!tsk->at_remote) {
		__make_pte_valid(mm, vma, addr, fault_flags, pte);
	} else  { /* bottom-half of localfault_at_remote */
		if (populated) {
			do_set_pte(vma, addr, page, pte, is_write, true);
			mem_cgroup_commit_charge(page, memcg, false);
			lru_cache_add_active_or_unevictable(page, vma);
			/* pf_action_record_remote_res_succ_write_grant */

			SetPageDistributed(mm, addr);
			set_page_owner(my_nid, mm, addr);
		} else {
			__make_pte_valid(mm, vma, addr, fault_flags, pte);
		}
	}

	pte_unmap(pte);

	put_page(page); /* origin doesn't need to put_page */

	ret = 0; /* PREFETCH_SUCCESS */

next:
	return;
}

/* IMPORTANT - int/process context */
struct remote_context *pfpg_fixup(void *msg, struct pcn_kmsg_rdma_handle *pf_handle)
{
    remote_prefetch_response_t *res =
                    (remote_prefetch_response_t*)msg;
    bool found = false;
	struct mm_struct *mm = NULL;
	struct remote_context *rc;
	struct task_struct *tsk = NULL;
    int pf_num = 0, succ_pf_num = 0;
#ifdef CONFIG_POPCORN_CHECK_SANITY
    BUG_ON(!res);
#endif

	tsk = __get_task_struct(res->origin_pid);
	BUG_ON(!tsk);

	mm = tsk->mm;
#ifdef CONFIG_POPCORN_CHECK_SANITY
	BUG_ON(!mm);
#endif
	rc = mm->remote;
#ifdef CONFIG_POPCORN_CHECK_SANITY
	BUG_ON(!rc);
#endif

#ifdef CONFIG_POPCORN_CHECK_SANITY
	BUG_ON(!found);
#endif

	while (pf_num < res->pf_list_size) {
		__fixup_page2(res, pf_num, &succ_pf_num, tsk);
		pf_num++;
	}


#if STATIS
	for (pf_num = 0; pf_num < succ_pf_num; pf_num++)
		atomic_inc(&rc->pf_succ_cnt);
#endif

	put_task_struct(tsk);
#if PREFETCH_WRITE
    pcn_kmsg_unpin_rdma_buffer(pf_handle);
#endif
	return rc;
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
	if (!pte_present(*pte)) {
		PCNPRINTK_ERR("Rmotefault_at_Remote: !pg_mine: "
						"addr %lx from [%d] writefault(%s) ins(%lx)\n", addr,
						req->remote_pid, fault_for_write(fault_flags)?"O":"X",
						req->instr_addr);
		BUG();
	}
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

	page = get_normal_page(vma, addr, pte);
	BUG_ON(!page);

	if (leader) {
		pte_t entry;

		/* Prepare the page if it is not mine. This should be leader */
		PGPRINTK(" =[%d] %s%s %p\n",
				tsk->pid, page_is_mine(mm, addr) ? "origin " : "",
				test_page_owner(from_nid, mm, addr) ? "remote": "", fh);

		if (test_page_owner(from_nid, mm, addr)) {
			if (fault_for_read(fault_flags)) {
				PCNPRINTK_ERR("Read fault from owner???: addr %lx pg_mine(%s): "
								"from [%d] writefault(%s) ins(%lx)\n",
				addr, page_is_mine(mm, addr)?"O":"X",
				req->remote_pid, fault_for_write(fault_flags)?"O":"X",
				req->instr_addr);
				BUG_ON(fault_for_read(fault_flags) && "Read fault from owner??");
			}
			__claim_local_page(tsk, addr, from_nid);
			grant = true;
		} else {
			if (!page_is_mine(mm, addr)) {
				/* This is impossible in 2 node case (happens for heap)(stack?) */
				PCNPRINTK_ERR("RR: addr %lx from [%d] writefault(%s) "
								"ins(%lx) tso_region(%s)\n",
								addr, req->remote_pid,
								fault_for_write(fault_flags)?"O":"X",
								req->instr_addr,
								current->tso_region?"O":"X");
				BUG();
				__claim_remote_page(tsk, mm, vma, addr, fault_flags, page, 0); // redundant
			} else {
				if (fault_for_write(fault_flags))
					__claim_local_page(tsk, addr, my_nid);
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
#ifdef CONFIG_POPCORN_STAT_PGFAULTS
	int rr = 0;
	ktime_t fph_start = ktime_get();
#endif

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
#ifdef CONFIG_POPCORN_STAT_PGFAULTS
		if (res->result == 0)
			rr = 1;
#endif
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

	END_KMSG_WORK(req);
#ifdef CONFIG_POPCORN_STAT_PGFAULTS
	if (is_trace_memory_pattern) {
		if (rr) {
			ktime_t dt, fph_end = ktime_get();
			dt = ktime_sub(fph_end, fph_start);
			atomic64_add(ktime_to_ns(dt), &fph_ns);
			atomic64_inc(&fph_cnt);
		}
	}
#endif
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
		__claim_remote_page(current, mm, vma, addr, fault_flags, page, 0);

#if DBG_ADDR_ON
		if (addr == DBG_ADDR) {
		//|| (addr > DBG_ADDR_LOW && addr < DBG_ADDR_HIGH) ) {
			PCNPRINTK_ERR("keep: 0x%lx: [%d] writefault(%s) "
							"current->tso_region(%s) "
							"page_is_mine(%s)(R) "
							"pte_present(%s) "
							"page_is_mine_at_remote_for_ww(%s)(can R) "
							"pte_none(%s) "
							"ins(%lx) \n",
							addr,
							current->pid,
							fault_for_write(fault_flags)?"O":"X",
							current->tso_region?"O":"X",
							page_is_mine(mm, addr)?"O":"X",
							pte_present(*pte)?"O":"X",
							page_is_mine_at_remote_for_ww(*pte)?"O":"X",
							pte_none(*pte)?"O":"X",
							instruction_pointer(current_pt_regs()) );
		}
#endif

#if DBG_ADDR_RANGE_ON && !DBG_ADDR_ON
		if ((addr > DBG_ADDR_LOW && addr < DBG_ADDR_HIGH) ) {
			PCNPRINTK_ERR("keep: 0x%lx: [%d] writefault(%s) "
							"current->tso_region(%s) "
							"page_is_mine(%s)(R) "
							"pte_present(%s) "
							"page_is_mine_at_remote_for_ww(%s)(can R) "
							"pte_none(%s) "
							"ins(%lx) \n",
							addr,
							current->pid,
							fault_for_write(fault_flags)?"O":"X",
							current->tso_region?"O":"X",
							page_is_mine(mm, addr)?"O":"X",
							pte_present(*pte)?"O":"X",
							page_is_mine_at_remote_for_ww(*pte)?"O":"X",
							pte_none(*pte)?"O":"X",
							instruction_pointer(current_pt_regs()) );
		}
#endif

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
	bool delay_write = false;
	struct mem_cgroup *memcg;
	int ret = 0;

	struct fault_handle *fh;
	bool leader;
	remote_page_response_t *rp;
#ifdef CONFIG_POPCORN_STAT_PGFAULTS
	ktime_t fp_start, fpin_start;
	ktime_t dt, inv_end, inv_start;
#endif
#if PROOF_DBG
	int proof= 0;
#endif

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

#if PROOF_DBG || MULTI_WRITER_REMOTE_TEST
	if (current->tso_region) {
		if (page_is_mine_at_remote_for_ww(*pte)) {
			if (fault_for_write(fault_flags)) {
				if (!populated) {
#if PROOF_DBG
					proof = 1;
#endif

#if MULTI_WRITER_REMOTE_TEST
					current->accu_tso_wr_cnt++;
					if (current->tso_wr_cnt < MAX_WRITE_INV_BUFFERS) {
						if (!page) BUG();
						tso_wr_inc(vma, addr, page, ptl);
					}
#endif
	}	}	}	}
#endif

#if REMOTE_TSO
	/* functionize this (origin & remote are the same code) */
	if (current->tso_region) {
		if (page_is_mine_at_remote_for_ww(*pte)) {
			if (fault_for_write(fault_flags)) { // remote_wr (write fault w/ page)
				if (!populated) { // will bring page (pg_mine(1) for no-accessed page bug)
					BUG_ON(!pte_present(*pte)); /* covered by populated */
					/* remote cannot trust test_page_owner(); */
					if (current->tso_wr_cnt < MAX_WRITE_INV_BUFFERS) { // move
						tso_wr_inc(vma, addr, page, ptl); // return
						delay_write = true;
#if PROOF_DBG
						proof = 1;
#endif
					} else {
#if PERF_DBG
						PCNPRINTK_ERR("[%d] increase MAX_WRITE_INV_BUFFERS %ld "
										"to enjoy\n", current->pid,
							(long)(MAX_WRITE_INV_BUFFERS - current->tso_wr_cnt));
#endif
					}
					current->accu_tso_wr_cnt++;
				}
			}
		}
	} else { // still cnt for reverse vain (is_skipped_region)
		if (is_skipped_region(current))
			current->skip_wr_per_rw_cnt++;
	}
#endif

#if DBG_ADDR_ON
	if (addr == DBG_ADDR) {
	//if (addr == DBG_ADDR || (addr > DBG_ADDR_LOW && addr < DBG_ADDR_HIGH) ) {
		PCNPRINTK_ERR("0x%lx: [%d] writefault(%s) "
						"current->tso_region(%s) "
						"page_is_mine(%s) "
						"pte_present(%s) "
						"page_is_mine_at_remote_for_ww(%s)(can R) "
						"populated(%s) "
						"pte_none(%s) "
						"ins(%lx) \n",
						addr,
						current->pid,
						fault_for_write(fault_flags)?"O":"X",
						current->tso_region?"O":"X",
						page_is_mine(mm, addr)?"O":"X",
						pte_present(*pte)?"O":"X",
						page_is_mine_at_remote_for_ww(*pte)?"O":"X",
						populated?"O":"X",
						pte_none(*pte)?"O":"X",
						instruction_pointer(current_pt_regs()) );
	}
#endif

#ifdef CONFIG_POPCORN_STAT_PGFAULTS
	fp_start = fpin_start = inv_start = ktime_get();
#endif

	if (!delay_write) {
		rp = __fetch_page_from_origin(current, vma, addr, fault_flags, page);
			// -> __request_remote_page() (collect read fault inside )
			// remote doesn't clear local meta.............
			// check permission
	} else { /* tso */
		rp = kzalloc(sizeof(*rp), GFP_KERNEL);
		rp->result = VM_FAULT_CONTINUE;
	}

#ifdef CONFIG_POPCORN_STAT_PGFAULTS
	if (is_trace_memory_pattern) {
		if (!delay_write) {
			if (pte_present(*pte)) {
				if (fault_for_write(fault_flags)) {
					if (rp->result == VM_FAULT_CONTINUE) { /* W: inv lat */
						inv_end = ktime_get();
						dt = ktime_sub(inv_end, inv_start);
						atomic64_add(ktime_to_ns(dt), &inv_ns);
						atomic64_inc(&inv_cnt);
					} else if (!rp->result) { /* W: inv + page transferred */
						// X -> W
						ktime_t dt, fpin_end = ktime_get();
						dt = ktime_sub(fpin_end, fpin_start);
						atomic64_add(ktime_to_ns(dt), &fpin_ns);
						atomic64_inc(&fpin_cnt);
					}
				}
			} else { /* fp only page */
				if (fault_for_read(fault_flags)) {
					ktime_t dt, fp_end = ktime_get();;
					dt = ktime_sub(fp_end, fp_start);
					atomic64_add(ktime_to_ns(dt), &fp_ns);
					atomic64_inc(&fp_cnt);
				}
				if (fault_for_write(fault_flags)) { /* W: inv + page transferred */
						ktime_t dt, fpin_end = ktime_get();
						dt = ktime_sub(fpin_end, fpin_start);
						atomic64_add(ktime_to_ns(dt), &fpin_ns);
						atomic64_inc(&fpin_cnt);
				}
			}
		}
	}
#endif

	if (rp->result && rp->result != VM_FAULT_CONTINUE) {
		if (rp->result != VM_FAULT_RETRY)
			PGPRINTK("  [%d] failed 0x%x\n", current->pid, rp->result);
		ret = rp->result;
		pte_unmap(pte);
		up_read(&mm->mmap_sem);
#if PROOF_DBG
		if (proof) {
			PCNPRINTK_ERR("%s: !proof %lx rp->ret error %d popu(%s)\n",
							__func__, addr, rp->result, populated?"O":"X");
			BUG();
		}
#endif
		goto out_free;
	}

	if (rp->result == VM_FAULT_CONTINUE) {
		/**
		 * Page ownership is granted without transferring the page data
		 * since this node already owns the up-to-dated page
		 */
		pte_t entry;
		BUG_ON(populated);

		spin_lock(ptl);
		entry = pte_make_valid(*pte);

		if (fault_for_write(fault_flags)) {
			entry = pte_mkwrite(entry);
			entry = pte_mkdirty(entry);
		} else {
#if PROOF_DBG
			if (proof) BUG();
#endif
			entry = pte_wrprotect(entry);
		}
		entry = pte_mkyoung(entry);

		if (ptep_set_access_flags(vma, addr, pte, entry, 1)) {
			update_mmu_cache(vma, addr, pte);
		}
	} else { /* !rp->result success and page brougt back */
#if PROOF_DBG
		if (proof) { // lud, cg(first pte_none then here)
			if (populated) {
				SYNCPRINTK2("%s: !proof %lx handle populated case "
							"*dealed*\n", __func__, addr);
			} else {
				PCNPRINTK_ERR("%s: !proof %lx (not populated)\n",
								__func__, addr);
				BUG();
			}
			/* pg_min + writefault => still page transferr
				any oter symptom? or init val of page_is_mine
				put _ERR addr & other addr in pg_mine() to check
				am i right about pg_mine() init val on remote is wrong */
		}
#endif
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
	set_page_owner(my_nid, mm, addr); // BUG() not ready
	pte_unmap_unlock(pte, ptl);
	ret = 0;	/* The leader squashes both 0 and VM_FAULT_CONTINUE to 0 */

out_free:
	put_page(page);
	if (!delay_write)
		pcn_kmsg_done(rp);
	else
		kfree(rp);
	fh->ret = ret;

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
	bool delay_write = false;

	struct fault_handle *fh;
	bool leader;
#ifdef CONFIG_POPCORN_STAT_PGFAULTS
	bool remote_fault = false;
	//ktime_t ptef_start = ktime_get();
#endif

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

#if MULTI_WRITER_ORIGIN_TEST
		if (current->tso_region) {
		/*full auto currently doesn't have a id*/
			if (test_page_owner(1, mm, addr)) {
				current->accu_tso_wr_cnt++;
				if (current->tso_wr_cnt < MAX_WRITE_INV_BUFFERS) {
					struct page *page = vm_normal_page(vma, addr, pte_val);
					if (!page) BUG();
					tso_wr_inc(vma, addr, page, ptl);
		}	}	}

#endif
		/* functionize this (origin & remote are the same code) */
		// origin_wr (write fault w/ page)
#if ORIGIN_TSO
		if (current->tso_region) {
			/* tso - inv */
			if (!pte_present(pte_val)) {
				PCNPRINTK_ERR("%s: origin !pte_present %lx handle it\n",
								__func__, addr);
			}

			// make sure remote own the page
			if (test_page_owner(1, mm, addr)) {
				current->accu_tso_wr_cnt++;
				if (current->tso_wr_cnt < MAX_WRITE_INV_BUFFERS) {
														/* move */
					struct page *page = vm_normal_page(vma, addr, pte_val);
					if (!page) BUG();
					tso_wr_inc(vma, addr, page, ptl);
											/* implement return */
					delay_write = true;
#if DBG_ADDR_ON
					if (addr == DBG_ADDR)
						SYNCPRINTK("[%d] origin buf %lx W(%s) just (info)\n",
									current->pid, addr,
									fault_for_write(fault_flags)?"O":"X");
#endif
				} else {
#if PERF_DBG
					PCNPRINTK_ERR("[%d] inc MAX_WRITE_INV_BUFFERS %ld to enjoy\n",
									current->pid,
							(long)(MAX_WRITE_INV_BUFFERS - current->tso_wr_cnt));
#endif
				}
			} else {
				// I have handled it. But someone change the permission again.
				// we have handled, don't try to put it into hashset again (overheaod)
				// The remote doesn't use ownership to do this so nothing to do
				//delay_write = true;
			}
		} else { // still cnt for reverse vain (is_skipped_region)
			if (is_skipped_region(current))
				current->skip_wr_per_rw_cnt++;
		}
#endif

#if DBG_ADDR_ON
	if (addr == DBG_ADDR) {
		if (current->tso_region) {
			PCNPRINTK_ERR("0x%lx: [%d] writefault(%s) "
							"current->tso_region(%s) "
							"page_is_mine(%s) "
							"pte_present(%s) "
							"pte_none(%s) "
							"ins(%lx) \n",
							addr,
							current->pid,
							fault_for_write(fault_flags)?"O":"X",
							current->tso_region?"O":"X",
							page_is_mine(mm, addr)?"O":"X",
							pte_present(pte_val)?"O":"X",
							pte_none(pte_val)?"O":"X",
							instruction_pointer(current_pt_regs()));
		} else {
			printk("0x%lx: [%d] writefault(%s) "
							"current->tso_region(%s) "
							"page_is_mine(%s) "
							"pte_present(%s) "
							"pte_none(%s) "
							"ins(%lx) \n",
							addr,
							current->pid,
							fault_for_write(fault_flags)?"O":"X",
							current->tso_region?"O":"X",
							page_is_mine(mm, addr)?"O":"X",
							pte_present(pte_val)?"O":"X",
							pte_none(pte_val)?"O":"X",
							instruction_pointer(current_pt_regs()));
		}
	}
#endif

		if (!delay_write)
			__claim_local_page(current, addr, my_nid);

		spin_lock(ptl);
		pte_val = pte_mkwrite(pte_val);
		pte_val = pte_mkdirty(pte_val);
		pte_val = pte_mkyoung(pte_val);

		if (ptep_set_access_flags(vma, addr, pte, pte_val, 1)) {
			update_mmu_cache(vma, addr, pte);
		}
	} else {
		struct page *page = vm_normal_page(vma, addr, pte_val);
#ifdef CONFIG_POPCORN_STAT_PGFAULTS
		ktime_t dt, clr_end, clr_start = ktime_get();
#endif
		BUG_ON(!page);

		__claim_remote_page(current, mm, vma, addr, fault_flags, page, 1);
#ifdef CONFIG_POPCORN_STAT_PGFAULTS
		if (is_trace_memory_pattern) {
			clr_end = ktime_get();
			dt = ktime_sub(clr_end, clr_start);
			atomic64_add(ktime_to_ns(dt), &clr_ns);
			atomic64_inc(&clr_cnt);
			remote_fault = true;
		}
#endif

		spin_lock(ptl);
		__make_pte_valid(mm, vma, addr, fault_flags, pte);
	}
#ifdef CONFIG_POPCORN_CHECK_SANITY
	BUG_ON(!test_page_owner(my_nid, mm, addr)); // Does this make send for NOCOPY?
#endif
	pte_unmap_unlock(pte, ptl);

out_wakeup:
	__finish_fault_handling(fh);

#ifdef CONFIG_POPCORN_STAT_PGFAULTS
	if (remote_fault) { /* this can be used for all/both cases */
	}
#endif

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


/**************************************************************************
 * Routing popcorn messages to workers
 */
DEFINE_KMSG_WQ_HANDLER(remote_page_request);
DEFINE_KMSG_WQ_HANDLER(page_invalidate_request);
DEFINE_KMSG_WQ_HANDLER(page_invalidate_batch_request);
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
	REGISTER_KMSG_WQ_HANDLER(PCN_KMSG_TYPE_PAGE_INVALIDATE_BATCH_REQUEST,
											page_invalidate_batch_request);
	REGISTER_KMSG_WQ_HANDLER(
			PCN_KMSG_TYPE_REMOTE_PAGE_FLUSH, remote_page_flush);
	REGISTER_KMSG_WQ_HANDLER(
			PCN_KMSG_TYPE_REMOTE_PAGE_RELEASE, remote_page_flush);
	REGISTER_KMSG_HANDLER(
			PCN_KMSG_TYPE_REMOTE_PAGE_FLUSH_ACK, remote_page_flush_ack);

	__fault_handle_cache = kmem_cache_create("fault_handle",
			sizeof(struct fault_handle), 0, 0, NULL);

	return 0;
}
