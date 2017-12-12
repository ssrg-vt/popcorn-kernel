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

static inline bool fault_for_write(unsigned long flags)
{
	return !!(flags & FAULT_FLAG_WRITE);
}

static inline bool fault_for_read(unsigned long flags)
{
	return !fault_for_write(flags);
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

static inline unsigned long *__get_page_info(struct mm_struct *mm, unsigned long addr)
{
	unsigned long key, offset;
	unsigned long *region;
	struct remote_context *rc = mm->remote;
	__get_page_info_key(addr, &key, &offset);

	region = radix_tree_lookup(&rc->pages, key);
	if (!region) return NULL;

	return region + offset;
}

void free_remote_context_pages(struct remote_context *rc)
{
	int nr_regions;
	const int FREE_BATCH = 16;
	unsigned long regions[FREE_BATCH];

	do {
		int i;

		nr_regions = radix_tree_gang_lookup(&rc->pages,
				(void **)regions, 0, FREE_BATCH);

		for (i = 0; i < nr_regions; i++) {
			struct page *page = virt_to_page(regions[i]);
			radix_tree_delete(&rc->pages, page_private(page));
			set_page_private(page, 0);
			free_page(regions[i]);
		}
	} while (nr_regions == FREE_BATCH);
}

#define FLAG_DISTRIBUTED 63

static inline bool SetPageDistributed(struct mm_struct *mm, unsigned long addr)
{
	unsigned long key, offset;
	unsigned long *region;
	struct remote_context *rc = mm->remote;
	__get_page_info_key(addr, &key, &offset);

	region = radix_tree_lookup(&rc->pages, key);
	if (!region) {
		int ret;
		struct page *page = alloc_page(GFP_ATOMIC | __GFP_ZERO);
		region = (unsigned long *)page_address(page);
		BUG_ON(!region);

		set_page_private(page, key);
		ret = radix_tree_insert(&rc->pages, key, region);
		BUG_ON(ret);
	}
	set_bit(FLAG_DISTRIBUTED, region + offset);

	return false;
}

static inline void ClearPageDistributed(struct mm_struct *mm, unsigned long addr)
{
	unsigned long *pi = __get_page_info(mm, addr);

	if (!pi) return;
	clear_bit(FLAG_DISTRIBUTED, pi);
	bitmap_clear(pi, 0, MAX_POPCORN_NODES);
}

static inline bool PageDistributed(struct mm_struct *mm, unsigned long addr)
{
	unsigned long *pi = __get_page_info(mm, addr);

	if (!pi || !test_bit(FLAG_DISTRIBUTED, pi)) return false;
	return true;
}


static inline bool page_is_mine(struct mm_struct *mm, unsigned long addr)
{
	unsigned long *pi = __get_page_info(mm, addr);

	if (!pi || !test_bit(FLAG_DISTRIBUTED, pi)) return true;
	return test_bit(my_nid, pi);
}

static inline bool test_page_owner(int nid, struct mm_struct *mm, unsigned long addr)
{
	unsigned long *pi = __get_page_info(mm, addr);

	if (!pi) return false;
	return test_bit(nid, pi);
}

static inline void set_page_owner(int nid, struct mm_struct *mm, unsigned long addr)
{
	unsigned long *pi = __get_page_info(mm, addr);

	if (!pi) return;
	set_bit(nid, pi);
}

static inline void clear_page_owner(int nid, struct mm_struct *mm, unsigned long addr)
{
	unsigned long *pi = __get_page_info(mm, addr);

	if (!pi) return;
	clear_bit(nid, pi);
}

/**************************************************************************
 * Fault tracking mechanism
 */
enum {
	FAULT_HANDLE_WRITE = 0x01,
	FAULT_HANDLE_REMOTE = 0x02,
	FAULT_HANDLE_INVALIDATE = 0x04,

	FAULT_FLAG_REMOTE = 0x100,

	FAULT_COALESCE_MAX = 8,
};

static struct kmem_cache *__fault_handle_cache = NULL;

struct fault_handle {
	struct list_head list;

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
};

static struct fault_handle *__alloc_fault_handle(struct task_struct *tsk, unsigned long addr)
{
	struct fault_handle *fh =
			kmem_cache_alloc(__fault_handle_cache, GFP_ATOMIC);
	BUG_ON(!fh);

	INIT_LIST_HEAD(&fh->list);

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

	list_add(&fh->list, &fh->rc->faults);
	return fh;
}


static struct fault_handle *__start_invalidation(struct task_struct *tsk, unsigned long addr, spinlock_t *ptl)
{
	unsigned long flags;
	struct remote_context *rc = get_task_remote(tsk);
	struct fault_handle *fh;
	bool found = false;
	DECLARE_COMPLETION_ONSTACK(complete);

	spin_lock_irqsave(&rc->faults_lock, flags);
	list_for_each_entry(fh, &rc->faults, list) {
		if (fh->addr == addr) {
			PGPRINTK("  [%d] %s %s ongoing, wait\n", tsk->pid,
				fh->flags & FAULT_HANDLE_REMOTE ? "remote" : "local",
				fh->flags & FAULT_HANDLE_WRITE ? "write" : "read");
			BUG_ON(fh->flags & FAULT_HANDLE_INVALIDATE);
			fh->flags |= FAULT_HANDLE_INVALIDATE;
			fh->complete = &complete;
			found = true;
			break;
		}
	}
	spin_unlock_irqrestore(&rc->faults_lock, flags);
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

	if (!fh) return;

	BUG_ON(atomic_read(&fh->pendings));
	spin_lock_irqsave(&fh->rc->faults_lock, flags);
	list_del(&fh->list);
	spin_unlock_irqrestore(&fh->rc->faults_lock, flags);

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
	char *ongoing = NULL;
	DEFINE_WAIT(wait);

	spin_lock_irqsave(&rc->faults_lock, flags);
	spin_unlock(ptl);

	list_for_each_entry(fh, &rc->faults, list) {
		if (fh->addr == addr) {
			found = true;
			break;
		}
	}

	if (found) {
		/* Invalidation cannot be merged with others */
		if (fh->flags & FAULT_HANDLE_INVALIDATE) {
			ongoing = "invalidate";
			goto out_wait;
		}

		/* Remote fault cannot be coalesced with others */
		if (fh->flags & FAULT_HANDLE_REMOTE) {
			ongoing = "remote";
			if (fault_flags & FAULT_FLAG_REMOTE) {
				goto out_retry;
			}
			goto out_wait;
		}
		if (fault_flags & FAULT_FLAG_REMOTE) {
			ongoing = "local";
			if (!tsk->at_remote) {
				goto out_retry;
			}
			goto out_wait;
		}

		/* Different fault types cannot be coalesced */
		if (fault_for_write(fault_flags) ^ !!(fh->flags & FAULT_HANDLE_WRITE)) {
			ongoing = (fh->flags & FAULT_HANDLE_WRITE) ? "write" : "read";
			goto out_wait;
		}

		if (fh->limit++ > FAULT_COALESCE_MAX) {
			ongoing = "max";
			goto out_wait;
		}

		atomic_inc(&fh->pendings);
		prepare_to_wait_exclusive(&fh->waits, &wait, TASK_UNINTERRUPTIBLE);
		spin_unlock_irqrestore(&rc->faults_lock, flags);
		PGPRINTK(" +[%d] %lx %p\n", tsk->pid, addr, fh);
		put_task_remote(tsk);

		io_schedule();
		smp_rmb();
		finish_wait(&fh->waits, &wait);

		fh->pid = tsk->pid;
		*leader = false;
		return fh;
	}

	fh = __alloc_fault_handle(tsk, addr);
	fh->flags |= fault_for_write(fault_flags) ? FAULT_HANDLE_WRITE : 0;
	fh->flags |= (fault_flags & FAULT_FLAG_REMOTE) ? FAULT_HANDLE_REMOTE : 0;

	spin_unlock_irqrestore(&rc->faults_lock, flags);
	put_task_remote(tsk);

	*leader = true;
	return fh;

out_wait:
	atomic_inc(&fh->pendings_retry);
	prepare_to_wait(&fh->waits_retry, &wait, TASK_UNINTERRUPTIBLE);
	spin_unlock_irqrestore(&rc->faults_lock, flags);
	put_task_remote(tsk);

	PGPRINTK("  [%d] %s ongoing. waits %p\n", tsk->pid, ongoing, fh);
	io_schedule();
	finish_wait(&fh->waits_retry, &wait);
	if (atomic_dec_and_test(&fh->pendings_retry)) {
		kmem_cache_free(__fault_handle_cache, fh);
	}
	return NULL;

out_retry:
	spin_unlock_irqrestore(&rc->faults_lock, flags);
	put_task_remote(tsk);

	PGPRINTK("  [%d] %s locked. retry %p\n", tsk->pid, ongoing, fh);
	return NULL;
}

static bool __finish_fault_handling(struct fault_handle *fh)
{
	unsigned long flags;
	bool last = false;

	spin_lock_irqsave(&fh->rc->faults_lock, flags);
	if (atomic_dec_return(&fh->pendings)) {
		PGPRINTK(" >[%d] %lx %p\n", fh->pid, fh->addr, fh);
		wake_up(&fh->waits);
	} else {
		PGPRINTK(">>[%d] %lx %p\n", fh->pid, fh->addr, fh);
		if (fh->complete) {
			complete(fh->complete);
		} else {
			list_del(&fh->list);
			last = true;
		}
	}
	spin_unlock_irqrestore(&fh->rc->faults_lock, flags);

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
	struct pcn_kmsg_work *w = (struct pcn_kmsg_work *)work;
	remote_page_flush_t *req = w->msg;
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
		.header = {
			.type = PCN_KMSG_TYPE_REMOTE_PAGE_FLUSH_ACK,
			.prio = PCN_KMSG_PRIO_NORMAL,
		},
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
		pcn_kmsg_send(req->remote_nid, &res, sizeof(res));
		goto out_put;
	} else if (req->flags & FLUSH_FLAG_LAST) {
		res.flags = FLUSH_FLAG_LAST;
		pcn_kmsg_send(req->remote_nid, &res, sizeof(res));
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
	pcn_kmsg_free_msg(req);
	kfree(w);
}


static int __do_pte_flush(pte_t *pte, unsigned long addr, unsigned long next, struct mm_walk *walk)
{
	remote_page_flush_t *req = walk->private;
	struct vm_area_struct *vma = walk->vma;
	struct page *page;
	int req_size;
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

			req->header.type = PCN_KMSG_TYPE_REMOTE_PAGE_FLUSH;
			req_size = sizeof(remote_page_flush_t);
			req->flags = FLUSH_FLAG_FLUSH;
			type = '*';
		} else {
			req->header.type = PCN_KMSG_TYPE_REMOTE_PAGE_RELEASE;
			req_size = sizeof(remote_page_release_t);
			req->flags = FLUSH_FLAG_RELEASE;
			type = '+';
		}
		clear_page_owner(my_nid, vma->vm_mm, addr);

		pcn_kmsg_send(current->origin_nid, req, req_size);
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

	req->header.prio = PCN_KMSG_PRIO_NORMAL;

	req->remote_nid = my_nid;
	req->remote_pid = current->pid;
	req->remote_ws = ws->id;
	req->origin_pid = current->origin_pid;
	req->addr = 0;

	/* Notify the start synchronously */
	req->header.type = PCN_KMSG_TYPE_REMOTE_PAGE_RELEASE;
	req->flags = FLUSH_FLAG_START;
	pcn_kmsg_send(current->origin_nid, req, sizeof(*req));
	wait_at_station(ws);

	/* Send pages asynchronously */
	down_read(&mm->mmap_sem);
	for (vma = mm->mmap; vma; vma = vma->vm_next) {
		walk.vma = vma;
		walk_page_vma(vma, &walk);
	}
	up_read(&mm->mmap_sem);

	/* Notify the completion synchronously */
	req->header.type = PCN_KMSG_TYPE_REMOTE_PAGE_FLUSH;
	req->flags = FLUSH_FLAG_LAST;
	pcn_kmsg_send(current->origin_nid, req, sizeof(*req));
	wait_at_station(ws);

	kfree(req);
	put_wait_station(ws);

	// XXX: make sure there is no backlog.
	msleep(1000);

	return 0;
}

static int handle_remote_page_flush_ack(struct pcn_kmsg_message *msg)
{
	remote_page_flush_ack_t *req = (remote_page_flush_ack_t *)msg;
	struct wait_station *ws = wait_station(req->remote_ws);

	complete(&ws->pendings);

	pcn_kmsg_free_msg(req);
	return 0;
}


/**************************************************************************
 * Page invalidation protocol
 */

static void __do_invalidate_page(struct task_struct *tsk, page_invalidate_request_t *req)
{
	struct mm_struct *mm = get_task_mm(tsk);
	struct page *page;
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
			req->origin_pid, req->origin_nid);

	pte = __get_pte_at(mm, addr, &pmd, &ptl);
	if (!pte) goto out;

	spin_lock(ptl);
	fh = __start_invalidation(tsk, addr, ptl);

	page = vm_normal_page(vma, addr, *pte);
	BUG_ON(!page);
	get_page(page);
	clear_page_owner(my_nid, mm, addr);
	put_page(page);

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
	struct pcn_kmsg_work *w = (struct pcn_kmsg_work *)work;
	page_invalidate_request_t *req = w->msg;
	page_invalidate_response_t res = {
		.header = {
			.type = PCN_KMSG_TYPE_PAGE_INVALIDATE_RESPONSE,
			.prio = PCN_KMSG_PRIO_NORMAL,
		},
		.origin_pid = req->origin_pid,
		.origin_ws = req->origin_ws,
		.remote_pid = req->remote_pid,
	};
	struct task_struct *tsk;

	/* Only home issues invalidate requests. Hence, I am a remote */
	tsk = __get_task_struct(req->remote_pid);
	if (!tsk) {
		PGPRINTK("%s: no such process %d %d %lx\n", __func__,
				req->origin_pid, req->remote_pid, req->addr);
		goto out_free;
	}

	__do_invalidate_page(tsk, req);

	pcn_kmsg_send(req->origin_nid, &res, sizeof(res));
	PGPRINTK(">>[%d] ->[%d/%d]\n", req->remote_pid, res.origin_pid, res.origin_nid);

	put_task_struct(tsk);

out_free:
	pcn_kmsg_free_msg(req);
	kfree(w);
}


static int handle_page_invalidate_response(struct pcn_kmsg_message *msg)
{
	page_invalidate_response_t *res = (page_invalidate_response_t *)msg;
	struct wait_station *ws = wait_station(res->origin_ws);

	if (atomic_dec_and_test(&ws->pendings_count)) {
		complete(&ws->pendings);
	}

	pcn_kmsg_free_msg(res);
	return 0;
}


static void __revoke_page_ownership(struct task_struct *tsk, int nid, pid_t pid, unsigned long addr, int ws_id)
{
	page_invalidate_request_t req = {
		.header = {
			.type = PCN_KMSG_TYPE_PAGE_INVALIDATE_REQUEST,
			.prio = PCN_KMSG_PRIO_NORMAL,
		},
		.addr = addr,
		.origin_nid = my_nid,
		.origin_pid = tsk->pid,
		.origin_ws = ws_id,
		.remote_pid = pid,
	};

	PGPRINTK("  [%d] revoke %lx [%d/%d]\n", tsk->pid, addr, pid, nid);
	pcn_kmsg_send(nid, &req, sizeof(req));
}



/**************************************************************************
 * Handle page faults happened at remote nodes.
 */
#ifdef CONFIG_POPCORN_KMSG_IB_RDMA
static remote_page_response_t *__fetch_remote_page_rdma(struct task_struct *tsk, int from_nid, pid_t from_pid, unsigned long addr, unsigned long fault_flags)
{
	remote_page_request_t req = {
		.header = {
			.type = PCN_KMSG_TYPE_REMOTE_PAGE_REQUEST,
			.prio = PCN_KMSG_PRIO_NORMAL,
		},
		.rdma_header = {
			.is_write = true,
		},

		.addr = addr,
		.fault_flags = fault_flags,

		.origin_nid = my_nid,
		.origin_pid = tsk->pid,

		.remote_pid = from_pid,
	};

	PGPRINTK("  [%d] ->[%d/%d] %lx\n", tsk->pid, from_pid, from_nid, addr);
	return pcn_kmsg_send_rdma(from_nid, &req, sizeof(req),
								sizeof(remote_page_response_t));
}
static remote_page_response_t *__fetch_page_from_origin(struct task_struct *tsk, unsigned long addr, unsigned long fault_flags)
{
	return __fetch_remote_page_rdma(tsk, tsk->origin_nid, tsk->origin_pid,
			addr, fault_flags);
}

#else /* CONFIG_POPCORN_KMSG_IB_RDMA */

static int handle_remote_page_response(struct pcn_kmsg_message *msg)
{
	remote_page_response_t *res = (remote_page_response_t *)msg;
	struct wait_station *ws = wait_station(res->origin_ws);

	PGPRINTK("  [%d] <-[%d/%d] %lx %x\n",
			ws->pid, res->remote_pid, res->remote_nid, res->addr, res->result);
	ws->private = res;

	smp_mb();
	if (atomic_dec_and_test(&ws->pendings_count))
		complete(&ws->pendings);
	return 0;
}

static void __request_remote_page(struct task_struct *tsk, int from_nid, pid_t from_pid, unsigned long addr, unsigned long fault_flags, int ws_id)
{
	remote_page_request_t req = {
		.header = {
			.type = PCN_KMSG_TYPE_REMOTE_PAGE_REQUEST,
			.prio = PCN_KMSG_PRIO_NORMAL,
		},

		.addr = addr,
		.fault_flags = fault_flags,

		.origin_nid = my_nid,
		.origin_pid = tsk->pid,
		.origin_ws = ws_id,

		.remote_pid = from_pid,
	};

	pcn_kmsg_send(from_nid, &req, sizeof(req));

	PGPRINTK("  [%d] ->[%d/%d] %lx\n", tsk->pid, from_pid, from_nid, addr);
}

static remote_page_response_t *__fetch_page_from_origin(struct task_struct *tsk, unsigned long addr, unsigned long fault_flags)
{
	remote_page_response_t *rp;
	struct wait_station *ws = get_wait_station(tsk);

	__request_remote_page(tsk, tsk->origin_nid, tsk->origin_pid,
			addr, fault_flags, ws->id);

	rp = wait_at_station(ws);
	put_wait_station(ws);

	return rp;
}
#endif /* CONFIG_POPCORN_KMSG_IB_RDMA */


static void __make_pte_valid(struct mm_struct *mm,
		struct vm_area_struct *vma, unsigned long addr,
		unsigned long fault_flags, pte_t *pte, struct page *page)
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

	set_pte_at_notify(mm, addr, pte, entry);
	update_mmu_cache(vma, addr, pte);
	// flush_tlb_page(vma, addr);

	SetPageDistributed(mm, addr);
	set_page_owner(my_nid, mm, addr);
}


static remote_page_response_t *__claim_remote_page(struct task_struct *tsk, unsigned long addr, unsigned long fault_flags, struct page *page)
{
	int peers;
	unsigned int random = prandom_u32();
	struct wait_station *ws;
	struct remote_context *rc = get_task_remote(tsk);
	remote_page_response_t *rp;
	int from, nid;
	int from_nid;
	unsigned long *pi = __get_page_info(rc->mm, addr);
	BUG_ON(!pi);

	peers = bitmap_weight(pi, MAX_POPCORN_NODES);

	if (test_bit(my_nid, pi)) {
		peers--;
	}
	from = random % peers;

	// PGPRINTK("  [%d] fetch %lx from %d peers\n", tsk->pid, addr, peers);

	if (fault_for_read(fault_flags)) {
		peers = 0;
	} else {
		peers--;
	}
#ifndef CONFIG_POPCORN_KMSG_IB_RDMA
	/* counting the fetch request */
	peers++;
#endif
	ws = get_wait_station_multiple(tsk, peers);

	for_each_set_bit(nid, pi, MAX_POPCORN_NODES) {
		pid_t pid = rc->remote_tgids[nid];
		if (nid == my_nid) continue;
		if (from-- == 0) {
			from_nid = nid;
#ifdef CONFIG_POPCORN_KMSG_IB_RDMA
			rp = __fetch_remote_page_rdma(tsk, nid, pid, addr, fault_flags);
#else
			__request_remote_page(tsk, nid, pid, addr, fault_flags, ws->id);
#endif
			if (fault_for_read(fault_flags)) break;
			continue;
		}
		if (fault_for_write(fault_flags)) {
			clear_bit(nid, pi);
			__revoke_page_ownership(tsk, nid, pid, addr, ws->id);
		}
	}

#ifdef CONFIG_POPCORN_KMSG_IB_RDMA
	if (peers) wait_at_station(ws);
#else
	rp = wait_at_station(ws);
#endif
	put_wait_station(ws);

	if (fault_for_write(fault_flags)) {
		clear_bit(from_nid, pi);
	}

	put_task_remote(tsk);
	return rp;
}


static void __claim_local_page(struct task_struct *tsk, unsigned long addr, struct page *page, int except_nid)
{
	struct mm_struct *mm = tsk->mm;
	unsigned long *pi = __get_page_info(mm, addr);
	int peers;

	if (!pi) return; /* skip claiming non-distributed page */
	peers = bitmap_weight(pi, MAX_POPCORN_NODES);
	if (!peers) return;	/* skip claiming the page that is not distributed */

	BUG_ON(!test_bit(except_nid, pi));
	peers--;	/* exclude except_nid from peers */

	if (test_bit(my_nid, pi) && except_nid != my_nid) peers--;

	if (peers > 0) {
		int nid;
		struct remote_context *rc = get_task_remote(tsk);
		struct wait_station *ws = get_wait_station_multiple(tsk, peers);

		for_each_set_bit(nid, pi, MAX_POPCORN_NODES) {
			pid_t pid = rc->remote_tgids[nid];
			if (nid == except_nid || nid == my_nid) continue;

			clear_bit(nid, pi);
			__revoke_page_ownership(tsk, nid, pid, addr, ws->id);
		}
		put_task_remote(tsk);

		wait_at_station(ws);
		put_wait_station(ws);
	}
}


void page_server_zap_pte(struct vm_area_struct *vma, unsigned long addr, pte_t *pte, pte_t *pteval)
{
	struct page *page;

	if (!vma->vm_mm->remote) return;

	ClearPageDistributed(vma->vm_mm, addr);
	page = vm_normal_page(vma, addr, *pte);
	if (!page) return;

	*pteval = pte_make_valid(*pte);
	*pteval = pte_mkyoung(*pteval);
	if (ptep_set_access_flags(vma, addr, pte, *pteval, 1)) {
		update_mmu_cache(vma, addr, pte);
	}
#ifdef CONFIG_POPCORN_DEBUG_VERBOSE
	PGPRINTK("  [%d] zap %lx\n", current->pid, addr);
#endif
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

	page = vm_normal_page(vma, addr, *pte);
	BUG_ON(!page);
	get_page(page);

	BUG_ON(!page_is_mine(mm, addr));

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

	flush_cache_page(vma, addr, page_to_pfn(page));
	paddr = kmap_atomic(page);
	copy_from_user_page(vma, page, addr, res->page, paddr, PAGE_SIZE);
	kunmap_atomic(paddr);

	pte_unmap_unlock(pte, ptl);

	put_page(page);
	__finish_fault_handling(fh);

	return 0;
}



/**************************************************************************
 * Remote fault handler at the origin
 */
static int __handle_remotefault_at_origin(struct task_struct *tsk, struct mm_struct *mm, struct vm_area_struct *vma, remote_page_request_t *req, remote_page_response_t *res)
{
	int from_nid = req->origin_nid;
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
		PGPRINTK("  [%d] handle local fault at origin\n", tsk->pid);
		spin_unlock(ptl);
		ret = handle_pte_fault_origin(mm, vma, addr, pte, pmd, fault_flags);
		if (ret & VM_FAULT_RETRY) {
			/* mmap_sem is released during do_fault */
			return VM_FAULT_RETRY;
		}
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
	get_page(page);

	if (leader) {
		pte_t entry;

		/* Prepare the page if it is not mine. This should be leader */
		PGPRINTK(" =[%d] %s%s %p\n",
				tsk->pid, page_is_mine(mm, addr) ? "origin " : "",
				test_page_owner(from_nid, mm, addr) ? "remote": "", fh);

		if (test_page_owner(from_nid, mm, addr)) {
			BUG_ON(fault_for_read(fault_flags) && "Read fault from owner??");
			__claim_local_page(tsk, addr, page, from_nid);
			grant = true;
		} else  {
			if (!page_is_mine(mm, addr)) {
				remote_page_response_t *rp =
					__claim_remote_page(tsk, addr, fault_flags, page);

				paddr = kmap(page);
				copy_to_user_page(vma, page, addr, paddr, rp->page, PAGE_SIZE);
				kunmap(page);

				pcn_kmsg_free_msg(rp);
			} else {
				if (fault_for_write(fault_flags))
					__claim_local_page(tsk, addr, page, my_nid);
			}
		}
		spin_lock(ptl);
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

		SetPageDistributed(mm, addr);
		set_page_owner(from_nid, mm, addr);
	} else {
		spin_lock(ptl);
	}

	if (!grant) {
		flush_cache_page(vma, addr, page_to_pfn(page));
		paddr = kmap_atomic(page);
		copy_from_user_page(vma, page, addr, res->page, paddr, PAGE_SIZE);
		kunmap_atomic(paddr);
	}

	pte_unmap_unlock(pte, ptl);

	__finish_fault_handling(fh);
	put_page(page);

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
	struct pcn_kmsg_work *w = (struct pcn_kmsg_work *)work;
	remote_page_request_t *req = w->msg;
	remote_page_response_t *res = NULL;
	struct task_struct *tsk;
	struct mm_struct *mm;
	struct vm_area_struct *vma;
	int res_size;
	int down_read_retry = 0;

	while (!res) {	/* response contains a page. allocate from a heap */
		res = kmalloc(sizeof(*res), GFP_KERNEL);
	}

again:
	tsk = __get_task_struct(req->remote_pid);
	if (!tsk) {
		res->result = VM_FAULT_SIGBUS;
		PGPRINTK("  [%d] not found\n", req->remote_pid);
		goto out;
	}
	mm = get_task_mm(tsk);

	PGPRINTK("\nREMOTE_PAGE_REQUEST [%d] %lx %c from [%d/%d]\n",
			req->remote_pid, req->addr,
			fault_for_write(req->fault_flags) ? 'W' : 'R',
			req->origin_pid, req->origin_nid);

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
	if (res->result == 0) {
		res->header.type = PCN_KMSG_TYPE_REMOTE_PAGE_RESPONSE;
		res_size = sizeof(remote_page_response_t);
	} else {
		res->header.type = PCN_KMSG_TYPE_REMOTE_PAGE_RESPONSE_SHORT;
		res_size = sizeof(remote_page_response_short_t);
	}
	res->header.prio = PCN_KMSG_PRIO_NORMAL;

	res->addr = req->addr;
	res->remote_nid = my_nid;
	res->remote_pid = req->remote_pid;

	res->origin_nid = req->origin_nid;
	res->origin_pid = req->origin_pid;
	res->origin_ws = req->origin_ws;

#ifdef CONFIG_POPCORN_KMSG_IB_RDMA
	pcn_kmsg_handle_rdma_at_remote(req, res, res_size);
#else
	pcn_kmsg_send(req->origin_nid, res, res_size);
#endif
	PGPRINTK("  [%d] ->[%d/%d] %x\n", req->remote_pid,
			res->origin_pid, res->origin_nid, res->result);

	trace_printk("%lx %c %d\n", req->addr,
			fault_for_write(req->fault_flags) ? 'W' : 'R', res->result);

	kfree(res);

	pcn_kmsg_free_msg(req);
	kfree(w);
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

	struct page *page;
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
	printk(" %c[%d] gup %s %p %p\n", leader ? '=' : '-', current->pid, mode,
		fh, uaddr);
	*/
	if (!leader) {
		ret = 0;
		pte_unmap(pte);
		goto out;
	}

	page = get_normal_page(vma, addr, pte);
	get_page(page);

	if (!page_is_mine(mm, addr)) {
		remote_page_response_t *rp =
				__claim_remote_page(current, addr, fault_flags, page);
		void *paddr;

		paddr = kmap(page);
		copy_to_user_page(vma, page, addr, paddr, rp->page, PAGE_SIZE);
		kunmap(page);
		SetPageUptodate(page);

		spin_lock(ptl);
		__make_pte_valid(mm, vma, addr, fault_flags, pte, page);
		pcn_kmsg_free_msg(rp);
		spin_unlock(ptl);
	}
	pte_unmap(pte);
	put_page(page);
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
	int ret = 0;

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

	rp = __fetch_page_from_origin(current, addr, fault_flags);

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
		BUG_ON(populated);

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
		void *paddr;
		paddr = kmap(page);
		copy_to_user_page(vma, page, addr, paddr, rp->page, PAGE_SIZE);
		kunmap(page);
		SetPageUptodate(page);

		spin_lock(ptl);
		if (populated) {
			do_set_pte(vma, addr, page, pte, fault_for_write(fault_flags), true);
			mem_cgroup_commit_charge(page, memcg, false);
			lru_cache_add_active_or_unevictable(page, vma);
		} else {
			__make_pte_valid(mm, vma, addr, fault_flags, pte, page);
		}
	}
	SetPageDistributed(mm, addr);
	set_page_owner(my_nid, mm, addr);
	pte_unmap_unlock(pte, ptl);
	ret = 0;	/* The leader squashes both 0 and VM_FAULT_CONTINUE to 0 */

out_free:
	put_page(page);
	pcn_kmsg_free_msg(rp);
	fh->ret = ret;

out_follower:
	__finish_fault_handling(fh);
	return ret;
}


/**************************************************************************
 * Local fault handler at the origin
 */
static int __handle_localfault_at_origin(struct mm_struct *mm,
		struct vm_area_struct *vma, unsigned long addr,
		pmd_t *pmd, pte_t *pte, pte_t pte_val, unsigned int fault_flags)
{
	struct page *page;
	spinlock_t *ptl;

	struct fault_handle *fh;
	bool leader;

	ptl = pte_lockptr(mm, pmd);
	spin_lock(ptl);

	/* Fresh access to the address. Handle locally since we are at the origin */
	if (pte_none(pte_val)) {
		BUG_ON(pte_present(pte_val));
		pte_unmap_unlock(pte, ptl);
		PGPRINTK("  [%d] fresh at origin. continue\n", current->pid);
		return VM_FAULT_CONTINUE;
	}

	if (!pte_same(*pte, pte_val)) {
		pte_unmap_unlock(pte, ptl);
		PGPRINTK("  [%d] %lx already handled\n", current->pid, addr);
		return 0;
	}

	page = vm_normal_page(vma, addr, pte_val);
	if (page == NULL || !PageDistributed(mm, addr)) {
		spin_unlock(ptl);
		/* Nothing to do with DSM (e.g. COW). Handle locally */
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
	get_page(page);

	PGPRINTK(" %c[%d] %lx replicated %smine %p\n",
			leader ? '=' : ' ', current->pid, addr,
			page_is_mine(mm, addr) ? "" : "not ", fh);

	if (!leader) {
		pte_unmap(pte);
		goto out_wakeup;
	}

	if (page_is_mine(mm, addr)) {
		if (fault_for_read(fault_flags)) {
			/* Racy exit */
			pte_unmap(pte);
			goto out_wakeup;
		}

		__claim_local_page(current, addr, page, my_nid);

		spin_lock(ptl);
		pte_val = pte_mkwrite(pte_val);
		pte_val = pte_mkdirty(pte_val);
		pte_val = pte_mkyoung(pte_val);

		if (ptep_set_access_flags(vma, addr, pte, pte_val, 1)) {
			update_mmu_cache(vma, addr, pte);
		}
	} else {
		remote_page_response_t *rp =
				__claim_remote_page(current, addr, fault_flags, page);
		void *paddr;

		BUG_ON(rp->result != 0);

		paddr = kmap(page);
		copy_to_user_page(vma, page, addr, paddr, rp->page, PAGE_SIZE);
		kunmap(page);
		SetPageUptodate(page);

		spin_lock(ptl);
		__make_pte_valid(mm, vma, addr, fault_flags, pte, page);
		pcn_kmsg_free_msg(rp);
	}
	BUG_ON(!test_page_owner(my_nid, mm, addr));
	pte_unmap_unlock(pte, ptl);

out_wakeup:
	put_page(page);
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
	trace_printk("%lx %c %lx %d\n",
			address, fault_for_write(fault_flags) ? 'W' : 'R',
			instruction_pointer(current_pt_regs()), ret);
	return ret;
}



/**************************************************************************
 * Routing popcorn messages to workers
 */

DEFINE_KMSG_WQ_HANDLER(remote_page_request);
DEFINE_KMSG_WQ_HANDLER(page_invalidate_request);
DEFINE_KMSG_ORDERED_WQ_HANDLER(remote_page_flush);

int __init page_server_init(void)
{
	REGISTER_KMSG_WQ_HANDLER(
			PCN_KMSG_TYPE_REMOTE_PAGE_REQUEST, remote_page_request);
#ifndef CONFIG_POPCORN_KMSG_IB_RDMA
	REGISTER_KMSG_HANDLER(
			PCN_KMSG_TYPE_REMOTE_PAGE_RESPONSE, remote_page_response);
	REGISTER_KMSG_HANDLER(
			PCN_KMSG_TYPE_REMOTE_PAGE_RESPONSE_SHORT, remote_page_response);
#endif
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

	__fault_handle_cache = kmem_cache_create("fault_handle",
			sizeof(struct fault_handle), 0, 0, NULL);

	return 0;
}
