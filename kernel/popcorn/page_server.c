/**
 * @file page_server.c
 *
 * Popcorn Linux page server implementation
 * This work was an extension of Marina Sadini MS Thesis, but totally revamped
 * for multiple node setup.
 *
 * @author Sang-Hoon Kim, SSRG Virginia Tech 2017
 */

#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/rmap.h>
#include <linux/mmu_notifier.h>
#include <linux/wait.h>
#include <linux/ptrace.h>
#include <linux/swap.h>
#include <linux/highmem.h>
#include <linux/pagemap.h>

#include <asm/tlbflush.h>
#include <asm/cacheflush.h>
#include <asm/mmu_context.h>
#include <asm/kdebug.h>

#include <popcorn/bundle.h>
#include <popcorn/cpuinfo.h>
#include <popcorn/pcn_kmsg.h>
#include <popcorn/debug.h>
#include <popcorn/process_server.h>

#include "types.h"
#include "stat.h"
#include "debug.h"

bool page_is_replicated(struct page *page)
{
	return !!bitmap_weight(page->owners, MAX_POPCORN_NODES);
}

bool page_is_mine(struct page *page)
{
	return !page_is_replicated(page) || test_bit(my_nid, page->owners);
}

struct remote_page {
	struct list_head list;
	unsigned long addr;

	bool mapped;
	int ret;
	atomic_t pendings;
	wait_queue_head_t pendings_wait;

	remote_page_response_t *response;
	DECLARE_BITMAP(owners, MAX_POPCORN_NODES);

	unsigned char data[PAGE_SIZE];
};

static struct remote_page *__alloc_remote_page_request(struct task_struct *tsk, unsigned long addr, unsigned long fault_flags, remote_page_request_t **preq)
{
	struct remote_page *rp = kmalloc(sizeof(*rp), GFP_KERNEL);
	remote_page_request_t *req = kmalloc(sizeof(*req), GFP_KERNEL);

	BUG_ON(!rp || !req);

	/* rp */
	INIT_LIST_HEAD(&rp->list);
	rp->addr = addr;

	rp->mapped = false;
	rp->ret = 0;
	atomic_set(&rp->pendings, 0);
	init_waitqueue_head(&rp->pendings_wait);

	/* req */
	req->header.type = PCN_KMSG_TYPE_REMOTE_PAGE_REQUEST;
	req->header.prio = PCN_KMSG_PRIO_NORMAL;

	req->remote_nid = my_nid;
	req->remote_pid = tsk->pid;
	req->origin_pid = tsk->origin_pid;
	req->addr = addr;
	req->fault_flags = fault_flags;

	*preq = req;

	return rp;
}


static struct remote_page *__lookup_pending_remote_page_request(struct remote_context *rc, unsigned long addr)
{
	struct remote_page *rp;

	list_for_each_entry(rp, &rc->pages, list) {
		if (rp->addr == addr) return rp;
	}
	return NULL;
}


static inline pte_t *__find_pte_lock(struct mm_struct *mm, unsigned long addr, spinlock_t **ptl)
{
	pgd_t *pgd;
	pud_t *pud;
	pmd_t *pmd;
	pte_t *pte;

	pgd = pgd_offset(mm, addr);
	pud = pud_alloc(mm, pgd, addr);
	if (!pud) return NULL;
	pmd = pmd_alloc(mm, pud, addr);
	if (!pmd) return NULL;

	pte = pte_alloc_map_lock(mm, pmd, addr, ptl);
	return pte;
}


static struct page *__find_page_at(struct mm_struct *mm, unsigned long addr, pte_t **ptep, spinlock_t **ptlp)
{
	pgd_t *pgd;
	pud_t *pud;
	pmd_t *pmd;
	pte_t *pte = NULL;
	spinlock_t *ptl = NULL;
	struct page *page;

	pgd = pgd_offset(mm, addr);
	if (!pgd || pgd_none(*pgd)) {
		return ERR_PTR(-VM_FAULT_SIGSEGV);
	}
	pud = pud_offset(pgd, addr);
	if (!pud || pud_none(*pud)) {
		return ERR_PTR(-VM_FAULT_SIGSEGV);
	}
	pmd = pmd_offset(pud, addr);
	if (!pmd || pmd_none(*pmd)) {
		return ERR_PTR(-VM_FAULT_SIGSEGV);
	}

	pte = pte_offset_map_lock(mm, pmd, addr, &ptl);
	if (pte == NULL || pte_none(*pte)) {
		// PTE not exist
		pte_unmap_unlock(pte, ptl);
		page = ERR_PTR(-VM_FAULT_SIGSEGV);
		pte = NULL;
		ptl = NULL;
	} else {
		page = pte_page(*pte);
		get_page(page);
	}

	*ptep = pte;
	*ptlp = ptl;
	return page;
}



/**************************************************************************
 * Flush pages to the origin
 */
static void process_remote_page_flush(struct work_struct *work)
{
	struct pcn_kmsg_work *w = (struct pcn_kmsg_work *)work;
	remote_page_flush_t *req = w->msg;
	struct task_struct *tsk;
	struct mm_struct *mm;
	struct page *page;
	pte_t *pte, entry;
	spinlock_t *ptl;
	void *paddr;
	struct vm_area_struct *vma;

	printk("%s: %lx from %d %d\n", __func__,
			req->addr, req->remote_pid, req->origin_pid);

	rcu_read_lock();
	tsk = find_task_by_vpid(req->origin_pid);
	if (!tsk) {
		rcu_read_unlock();
		goto out_free;
	}
	get_task_struct(tsk);
	mm = get_task_mm(tsk);
	rcu_read_unlock();

	if (req->last) {
		complete(&tsk->wait_for_remote_flush);
		goto out_put;
	}

	down_read(&mm->mmap_sem);
	vma = find_vma(mm, req->addr);
	BUG_ON(!vma || vma->vm_start > req->addr);

	page = __find_page_at(mm, req->addr, &pte, &ptl);
	if (IS_ERR(page)) {
		printk("%s: %lx\n", __func__, req->addr);
		BUG_ON(IS_ERR(page));
	}

	printk("%s: %lx %s\n", __func__, req->addr,
			pte_write(*pte) ? "writable" : "protected");

	paddr = kmap_atomic(page);
	copy_page(paddr, req->page);
	kunmap_atomic(paddr);
	set_bit(my_nid, page->owners);
	put_page(page);

	entry = pte_clear_flags(*pte, _PAGE_SOFTW1);
	entry = pte_set_flags(entry, _PAGE_PRESENT);
	set_pte_at(mm, req->addr, pte, entry);

	flush_tlb_page(vma, req->addr);

	pte_unmap_unlock(pte, ptl);
	up_read(&mm->mmap_sem);

out_put:
	put_task_struct(tsk);
	mmput(mm);

out_free:
	pcn_kmsg_free_msg(req);
	kfree(w);
}

static int __flush_pte(pte_t *pte, unsigned long addr, unsigned long next, struct mm_walk *walk)
{
	pte_t entry = *pte;
	remote_page_flush_t *req = walk->private;
	struct page *page;

	if (pte_none(entry)) return 0;

	page = pte_page(entry);

	if (test_bit(my_nid, page->owners)) {
		void *paddr;
		// TODO: Skip flushing read-only pages

		printk("flush_remote_page [%d]:+ %lx %p\n", current->pid, addr, page);
		req->addr = addr;
		paddr = kmap_atomic(page);
		copy_page(req->page, paddr);
		kunmap_atomic(paddr);

		clear_bit(my_nid, page->owners);

		pcn_kmsg_send_long(current->origin_nid, req, sizeof(*req));
	} else {
		printk("flush_remote_page [%d]:- %lx %p\n", current->pid, addr, page);
	}

	return 0;
}

int page_server_flush_remote_pages(struct remote_context *rc)
{
	remote_page_flush_t *req = kmalloc(sizeof(*req), GFP_KERNEL);
	struct mm_struct *mm = rc->mm;
	struct mm_walk walk = {
		.pte_entry = __flush_pte,
		.mm = mm,
		.private = req,
	};
	struct vm_area_struct *vma;

	BUG_ON(!req);

	req->header.type = PCN_KMSG_TYPE_REMOTE_PAGE_FLUSH;
	req->header.prio = PCN_KMSG_PRIO_NORMAL;

	req->remote_pid = current->pid;
	req->origin_pid = current->origin_pid;
	req->last = false;

	down_read(&mm->mmap_sem);
	for (vma = mm->mmap; vma; vma = vma->vm_next) {
		if (vma_is_anonymous(vma)) {
			// Need to sync
			walk.vma = vma;
			walk_page_vma(vma, &walk);
		} else {
			if (vma->vm_flags & VM_WRITE) {
				if (!(vma->vm_flags & VM_SHARED)) {
					walk.vma = vma;
					walk_page_vma(vma, &walk);
				}
			}
		}
	}
	up_read(&mm->mmap_sem);

	req->addr = 0;
	req->last = true;
	pcn_kmsg_send_long(current->origin_nid, req, sizeof(*req));

	kfree(req);

	return 0;
}



/**************************************************************************
 * Invalidate pages among distributed nodes
 */
static int __do_invalidate_page(struct task_struct *tsk, struct mm_struct *mm, unsigned long addr)
{
	struct page *page;
	struct vm_area_struct *vma;
	pte_t *pte, entry;
	spinlock_t *ptl;

	down_read(&mm->mmap_sem);
	vma = find_vma(mm, addr);
	if (!vma || vma->vm_start > addr) {
		up_read(&mm->mmap_sem);
		return VM_FAULT_SIGBUS;
	}

	page = __find_page_at(mm, addr, &pte, &ptl);
	if (IS_ERR(page)) {
		printk("inv: page does not exist at %lx\n", addr);
		up_read(&mm->mmap_sem);
		return -PTR_ERR(page);
	}
	clear_bit(my_nid, page->owners);

	entry = ptep_get_and_clear(mm, addr, pte);
	entry = pte_set_flags(entry, _PAGE_SOFTW1);
	entry = pte_clear_flags(entry, _PAGE_PRESENT);
	set_pte_at(mm, addr, pte, entry);

	flush_tlb_page(vma, addr);

	pte_unmap_unlock(pte, ptl);
	up_read(&mm->mmap_sem);

	put_page(page);

	return 0;
}

static void process_remote_page_invalidate(struct work_struct *_work)
{
	struct pcn_kmsg_work *w = (struct pcn_kmsg_work *)_work;
	remote_page_invalidate_t *req = w->msg;
	struct task_struct *tsk;
	struct mm_struct *mm;

	printk("invalidate_page: %lx\n", req->addr);

	/* Only home issues invalidate requests. Hence, I am a remote */
	rcu_read_lock();
	tsk = find_task_by_vpid(req->remote_pid);
	if (!tsk) {
		printk("%s: no such process %d %d\n", __func__,
				req->origin_pid, req->remote_pid);
		rcu_read_unlock();
		goto out_free;
	}
	get_task_struct(tsk);
	mm = get_task_mm(tsk);
	rcu_read_unlock();

	__do_invalidate_page(tsk, mm, req->addr);
	put_task_struct(tsk);
	mmput(mm);

out_free:
	pcn_kmsg_free_msg(req);
	kfree(w);
}


static void __revoke_page_ownership(struct task_struct *tsk, unsigned long addr, struct page *page, pte_t *pte, spinlock_t *ptl, int except)
{
	int nid;
	struct remote_context *rc = get_task_remote(tsk);
	remote_page_invalidate_t req = {
		.header.type = PCN_KMSG_TYPE_REMOTE_PAGE_INVALIDATE,
		.header.prio = PCN_KMSG_PRIO_NORMAL,
		.addr = addr,
		.origin_nid = my_nid,
		.origin_pid = tsk->pid,
	};

	/* Only the origin broadcasts the revocation message. */

	for_each_set_bit(nid, page->owners, MAX_POPCORN_NODES) {
		// TODO: accelerate self-invalidation
		if (nid == except) continue;

		req.remote_pid = rc->remote_tgids[nid];
		printk("revoke_ownership: 0x%lx at %d %d\n", addr, req.remote_pid, nid);
		pcn_kmsg_send_long(nid, &req, sizeof(req));
		clear_bit(nid, page->owners);
	}
	put_task_remote(tsk);
}



/**************************************************************************
 * Handle remote page fetch response
e*/

static void process_remote_page_response(struct work_struct *_work)
{
	struct pcn_kmsg_work *w = (struct pcn_kmsg_work *)_work;
	remote_page_response_t *res = w->msg;
	struct task_struct *tsk;
	unsigned long flags;
	struct remote_page *rp;
	struct remote_context *rc;

	rcu_read_lock();
	tsk = find_task_by_vpid(res->remote_pid);
	if (!tsk) {
		__WARN();
		rcu_read_unlock();
		goto out_free;
	}
	get_task_struct(tsk);
	rcu_read_unlock();
	rc = get_task_remote(tsk);

	spin_lock_irqsave(&rc->pages_lock, flags);
	rp = __lookup_pending_remote_page_request(rc, res->addr);
	spin_unlock_irqrestore(&rc->pages_lock, flags);
	put_task_remote(tsk);
	put_task_struct(tsk);
	if (!rp) {
		__WARN();
		goto out_free;
	}
	WARN_ON(atomic_read(&rp->pendings) <= 0);

	rp->response = res;
	wake_up(&rp->pendings_wait);

out_free:
	kfree(w);
}


/**************************************************************************
 * Handle for remote page fetch
 */
static int __forward_remote_page_request(struct task_struct *tsk, struct page *page, pte_t *pte, spinlock_t *ptl)
{
	struct remote_context *rc = get_task_remote(tsk);
	int nid;
	bool forwarded = false;

	pte_unmap_unlock(pte, ptl);

	WARN_ON("Not implemented yet");

	for_each_set_bit(nid, page->owners, MAX_POPCORN_NODES) {
		printk("owner is at %d %d\n", nid, rc->remote_tgids[nid]);
		forwarded = true;
	}

	WARN_ON(!forwarded && "no page owner");

	put_task_remote(tsk);
	return forwarded ? VM_FAULT_FORWARDED : VM_FAULT_SIGBUS;
}


static void __reply_remote_page(int nid, int remote_pid, remote_page_response_t *res)
{
	if (res->result == VM_FAULT_FORWARDED) return;

	res->header.type = PCN_KMSG_TYPE_REMOTE_PAGE_RESPONSE;
	res->header.prio = PCN_KMSG_PRIO_NORMAL;

	res->remote_pid = remote_pid;

	pcn_kmsg_send_long(nid, res, sizeof(*res));
}


static int __get_remote_page(struct task_struct *tsk, struct mm_struct *mm,
		struct vm_area_struct *vma, unsigned long addr,
		int from, remote_page_response_t *res, bool invalidate)
{
	int ret = 0;
	spinlock_t *ptl;
	pte_t *pte;
	struct page *page;
	void *paddr;

	page = __find_page_at(mm, addr, &pte, &ptl);
	if (IS_ERR(page)) {
		// TODO: should fall back to the host pte fault.
		return -PTR_ERR(page);
	}
	printk("remote_page_request: %s\n",
			pte_write(*pte) ? "writable" : "protected");

	if (!page_is_mine(page)) {
		// TODO: mark that this page is requested...??/
		ret = __forward_remote_page_request(tsk, page, pte, ptl);
		goto out_put;
	}

	if (invalidate) {
		__revoke_page_ownership(tsk, addr, page, pte, ptl, from);
	}

	paddr = kmap_atomic(page);
	copy_page(res->page, paddr);
	kunmap_atomic(paddr);

	set_bit(from, page->owners);
	memcpy(res->owners, page->owners, MAX_POPCORN_NODES);

	pte_unmap_unlock(pte, ptl);

out_put:
	put_page(page);
	return ret;
}


static void process_remote_page_request(struct work_struct *work)
{
	struct pcn_kmsg_work *w = (struct pcn_kmsg_work *)work;
	remote_page_request_t *req = w->msg;
	remote_page_response_t *res = NULL;
	struct task_struct *tsk;
	struct mm_struct *mm;
	struct vm_area_struct *vma;
	int from = req->remote_nid;

	might_sleep();

	while (!res) {	/* response contains a page. allocate from a heap */
		res = kmalloc(sizeof(*res), GFP_KERNEL);
	};
	res->addr = req->addr;
	printk("remote_page_request [%d]: %lx from [%d/%d]\n",
			req->origin_pid, req->addr, req->remote_pid, from);

	rcu_read_lock();
	tsk = find_task_by_vpid(req->origin_pid);
	if (!tsk) {
		res->result = VM_FAULT_SIGBUS;
		rcu_read_unlock();
		goto out;
	}
	get_task_struct(tsk);
	mm = get_task_mm(tsk);
	rcu_read_unlock();

	down_read(&mm->mmap_sem);
	vma = find_vma(mm, req->addr);

	if (!vma || vma->vm_start > req->addr) {
		res->result = VM_FAULT_SIGBUS;
		goto out_up;
	}

	if (!(req->fault_flags & FAULT_FLAG_WRITE)) {
		printk("remote_page_reqeust: read fault %d\n", from);
		res->result = __get_remote_page(tsk, mm, vma, req->addr, from, res, false);
	} else if (!(vma->vm_flags & VM_SHARED)) {
		printk("remote_page_request: write fault %d\n", from);
		res->result = __get_remote_page(tsk, mm, vma, req->addr, from, res, true);
	} else {
		BUG_ON("remote_page_request: shared fault\n");
	}
	printk("remote_page_request: vma at %lx -- %lx %lx\n",
			vma->vm_start, vma->vm_end, vma->vm_flags);

out_up:
	up_read(&mm->mmap_sem);
	mmput(mm);
	put_task_struct(tsk);

out:
	__reply_remote_page(from, req->remote_pid, res);
	pcn_kmsg_free_msg(req);
	kfree(w);
	kfree(res);
}


/**************************************************************************
 * Page fault handler
 */

static void __map_remote_page(struct remote_page *rp, unsigned long fault_flags)
{
	struct mm_struct *mm = current->mm;
	struct vm_area_struct *vma;
	remote_page_response_t *res = rp->response;
	unsigned long addr = res->addr;
	struct page *page;
	void *paddr;
	pte_t *pte;
	spinlock_t *ptl = NULL;
	struct mem_cgroup *memcg;

	down_read(&mm->mmap_sem);
	if ((rp->ret = res->result)) {
		// Something went wrong. Will not map this page
		return;
	}

	vma = find_vma(mm, addr);
	BUG_ON(!vma || vma->vm_start > addr);

	printk("%s: vma %lx -- %lx %lx\n", __func__,
			vma->vm_start, vma->vm_end, vma->vm_flags);

	if (vma_is_anonymous(vma)) {
		anon_vma_prepare(vma);
	}
	page = alloc_page_vma(GFP_HIGHUSER_MOVABLE, vma, addr);
	if (!page) {
		rp->ret = VM_FAULT_OOM;
		return;
	}
	if (mem_cgroup_try_charge(page, mm, GFP_KERNEL, &memcg)) {
		page_cache_release(page);
		rp->ret = VM_FAULT_OOM;
		return;
	}

	paddr = kmap_atomic(page);
	copy_page(paddr, rp->response->page);
	kunmap_atomic(paddr);

	memcpy(page->owners, res->owners, sizeof(page->owners));
	BUG_ON(!test_bit(my_nid, page->owners));

	__SetPageUptodate(page);
	mem_cgroup_commit_charge(page, memcg, false);
	lru_cache_add_active_or_unevictable(page, vma);

	pte = __find_pte_lock(mm, addr, &ptl);
	BUG_ON(!pte);

	do_set_pte(vma, addr, page, pte,
			fault_flags & FAULT_FLAG_WRITE, vma_is_anonymous(vma));

	/* no need to invalidate: a not-present page won't be cached */

	pte_unmap_unlock(pte, ptl);
}


static int __handle_remote_fault(unsigned long addr, unsigned long fault_flags)
{
	struct task_struct *tsk = current;
	struct remote_context *rc = get_task_remote(tsk);
	struct remote_page *rp;
	unsigned long flags;
	DEFINE_WAIT(wait);
	bool wakeup = false;
	int ret = 0;
	remote_page_request_t *req = NULL;
	int remaining = 0;

	// TODO: deal with the do_fault_around

	spin_lock_irqsave(&rc->pages_lock, flags);
	rp = __lookup_pending_remote_page_request(rc, addr);
	if (!rp) {
		struct remote_page *r;
		spin_unlock_irqrestore(&rc->pages_lock, flags);

		rp = __alloc_remote_page_request(tsk, addr, fault_flags, &req);

		spin_lock_irqsave(&rc->pages_lock, flags);
		r = __lookup_pending_remote_page_request(rc, addr);
		if (!r) {
			printk("%s [%d]: %lx [%d/%d]\n", __func__,
					tsk->pid, addr, tsk->origin_pid, tsk->origin_nid);
			list_add(&rp->list, &rc->pages);
		} else {
			printk("%s [%d]: %lx pended\n", __func__, tsk->pid, addr);
			kfree(rp);
			rp = r;
			kfree(req);
			req = NULL;
		}
	} else {
		printk("%s [%d]: %lx pended\n", __func__, tsk->pid, addr);
	}
	atomic_inc(&rp->pendings);
	prepare_to_wait(&rp->pendings_wait, &wait, TASK_UNINTERRUPTIBLE);
	spin_unlock_irqrestore(&rc->pages_lock, flags);
	up_read(&tsk->mm->mmap_sem);

	if (req) {
		pcn_kmsg_send_long(tsk->origin_nid, req, sizeof(*req));
		kfree(req);
	}

	io_schedule();

	/* Now the remote page would be brought in to rp */

	finish_wait(&rp->pendings_wait, &wait);

	if (!rp->mapped) {
		__map_remote_page(rp, fault_flags);	// This function set rp->ret
		rp->mapped = true;
	} else {
		down_read(&tsk->mm->mmap_sem);
	}
	ret = rp->ret;
	smp_mb();

	printk("%s [%d]: %lx resume %d [%d/%d]\n", __func__,
			tsk->pid, addr, ret, tsk->origin_pid, tsk->origin_nid);

	spin_lock_irqsave(&rc->pages_lock, flags);
	remaining = atomic_dec_return(&rp->pendings);
	if (remaining) {
		wakeup = true;
	} else {
		list_del(&rp->list);
		pcn_kmsg_free_msg(rp->response);
		kfree(rp);
	}
	spin_unlock_irqrestore(&rc->pages_lock, flags);
	put_task_remote(tsk);
	barrier();

	if (wakeup) wake_up(&rp->pendings_wait);

	return ret;
}

/**
 * down_read(&mm->mmap_sem) already held getting in
 *
 * return types:
 * VM_FAULT_CONTINUE, page_fault that can handled
 * 0, remotely fetched;
 */
int page_server_handle_pte_fault(struct mm_struct *mm,
		struct vm_area_struct *vma,
		unsigned long address, pte_t entry, pmd_t *pmd,
		unsigned int fault_flags)
{
	unsigned long addr = address & PAGE_MASK;

	might_sleep();

	printk(KERN_WARNING"\n");
	printk(KERN_WARNING"## PAGEFAULT [%d]: %lx %lx\n",
			current->pid, address, get_task_pc(current));

	if (!pte_present(entry)) {
		/* Do we need to create it locally or to bring from remote? */
		if (!pte_none(entry)) {
			WARN_ON("Claim the invalidated page.");
			return __handle_remote_fault(addr, fault_flags);
		}

		/* Can we handle the fault locally? */
		if (vma->vm_flags & VM_FETCH_LOCAL) {
			printk("pte_fault [%d]: VM_FETCH_LOCAL. continue\n", current->pid);
			return VM_FAULT_CONTINUE;
		}

		if (!vma_is_anonymous(vma) &&
				((vma->vm_flags & (VM_WRITE | VM_SHARED)) == 0)) {
			printk("pte_fault: Locally file-mapped read-only. continue\n");
			return VM_FAULT_CONTINUE;
		}
		return __handle_remote_fault(addr, fault_flags);
	}

	/* Fault occurs whereas PTE exists: cow? */
	if (fault_flags & FAULT_FLAG_WRITE) {
		if (!pte_write(entry)) {
			/* This page might be replicated by a read prior to current write.
			 * Claim this page from the server */
			printk("pte_fault: claim this page\n");
			return __handle_remote_fault(addr, fault_flags);
		} else {
			BUG_ON("pte_fault: write fault on writable page\n");
		}
	}
	printk("pte_fault: fall through for read\n");
	return 0;
}


/**************************************************************************
 * Routing popcorn messages to worker
 */
static struct workqueue_struct *remote_page_wq;

DEFINE_KMSG_WQ_HANDLER(remote_page_request, remote_page_wq);
DEFINE_KMSG_WQ_HANDLER(remote_page_response, remote_page_wq);
DEFINE_KMSG_WQ_HANDLER(remote_page_invalidate, remote_page_wq);
DEFINE_KMSG_WQ_HANDLER(remote_page_flush, remote_page_wq);

int __init page_server_init(void)
{
	remote_page_wq = create_workqueue("remote_page_wq");
	if (!remote_page_wq)
		return -ENOMEM;

	REGISTER_KMSG_WQ_HANDLER(
			PCN_KMSG_TYPE_REMOTE_PAGE_REQUEST, remote_page_request);
	REGISTER_KMSG_WQ_HANDLER(
			PCN_KMSG_TYPE_REMOTE_PAGE_RESPONSE, remote_page_response);
	REGISTER_KMSG_WQ_HANDLER(
			PCN_KMSG_TYPE_REMOTE_PAGE_INVALIDATE, remote_page_invalidate);
	REGISTER_KMSG_WQ_HANDLER(
			PCN_KMSG_TYPE_REMOTE_PAGE_FLUSH, remote_page_flush);

	return 0;
}
