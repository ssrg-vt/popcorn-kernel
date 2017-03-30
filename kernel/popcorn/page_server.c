/**
 * @file page_server.c
 *
 * Popcorn Linux page server implementation
 * This work was an extension of Marina Sadini MS Thesis, but totally revamped
 * for multiple node setup.
 *
 * @author Sang-Hoon Kim, SSRG Virginia Tech 2017
 */

#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/rmap.h>
#include <linux/mmu_notifier.h>
#include <linux/wait.h>
#include <linux/ptrace.h>
#include <linux/swap.h>
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
#include "pgtable.h"

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
	int peer_nid;
	unsigned long addr;

	bool mapped;
	int ret;
	atomic_t pendings;
	wait_queue_head_t pendings_wait;

	remote_page_response_t *response;
	unsigned char data[PAGE_SIZE];
};

static struct remote_page *__lookup_pending_remote_page_request(struct remote_context *rc, unsigned long addr)
{
	struct remote_page *rp;

	list_for_each_entry(rp, &rc->pages, list) {
		/*
		printk("LOOKUP [%d] %lx ?= %lx\n",
				current->pid, addr, rp->addr);
		*/
		if (rp->addr == addr) return rp;
	}
	return NULL;
}


static struct page *__find_page_at(struct mm_struct *mm, unsigned long addr, pte_t **ptep, spinlock_t **ptlp)
{
	pgd_t *pgd;
	pud_t *pud;
	pmd_t *pmd;
	pte_t *pte = NULL;
	spinlock_t *ptl = NULL;
	struct page *page = ERR_PTR(-ENOMEM);

	pgd = pgd_offset(mm, addr);
	if (!pgd || pgd_none(*pgd)) goto out;

	pud = pud_offset(pgd, addr);
	if (!pud || pud_none(*pud)) goto out;

	pmd = pmd_offset(pud, addr);
	if (!pmd || pmd_none(*pmd)) goto out;

	pte = pte_offset_map_lock(mm, pmd, addr, &ptl);
	if (pte == NULL || pte_none(*pte)) {
		// PTE not exist
		pte_unmap_unlock(pte, ptl);
		pte = NULL;
		ptl = NULL;
		page = ERR_PTR(-ENOENT);
		goto out;
	} else {
		page = pte_page(*pte);
		get_page(page);
	}

out:
	*ptep = pte;
	*ptlp = ptl;
	return page;
}

static struct remote_page *__alloc_remote_page_request(struct task_struct *tsk, struct remote_context *rc, unsigned long addr, unsigned long fault_flags, remote_page_request_t *req)
{
	struct remote_page *rp = kmalloc(sizeof(*rp), GFP_KERNEL);

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

	req->addr = addr;
	req->fault_flags = fault_flags;

	req->remote_nid = my_nid;
	req->remote_pid = tsk->pid;

	if (tsk->at_remote) {
		rp->peer_nid = tsk->origin_nid;
		req->origin_pid = tsk->origin_pid;
	} else {
		int nid;
		pte_t *ptep;
		spinlock_t *ptlp;
		struct mm_struct *mm = get_task_mm(tsk);
		struct page *page = __find_page_at(mm, addr, &ptep, &ptlp);
		BUG_ON(!page);

		nid = find_first_bit(page->owners, MAX_POPCORN_NODES);
		BUG_ON(nid >= MAX_POPCORN_NODES);

		req->origin_pid = rc->remote_tgids[nid];
		rp->peer_nid = nid;

		put_page(page);
		pte_unmap_unlock(ptep, ptlp);
		mmput(mm);
	}

	return rp;
}



/**************************************************************************
 * Flush pages to the origin
 */
static void process_remote_page_flush(struct work_struct *work)
{
	struct pcn_kmsg_work *w = (struct pcn_kmsg_work *)work;
	remote_page_flush_t *req = w->msg;
	unsigned long addr = req->addr;
	struct task_struct *tsk;
	struct mm_struct *mm;
	struct page *page;
	pte_t *pte, entry;
	spinlock_t *ptl;
	void *paddr;
	struct vm_area_struct *vma;

	printk("%s: %lx from %d %d\n", __func__,
			addr, req->remote_pid, req->origin_pid);

	tsk = __get_task_struct(req->origin_pid);
	if (!tsk) goto out_free;

	mm = get_task_mm(tsk);

	if (req->last) {
		complete(&tsk->wait_for_remote_flush);
		goto out_put;
	}

	down_read(&mm->mmap_sem);
	vma = find_vma(mm, addr);
	BUG_ON(!vma || vma->vm_start > addr);

	page = __find_page_at(mm, addr, &pte, &ptl);
	if (IS_ERR(page)) {
		printk("%s: %lx\n", __func__, addr);
		BUG_ON(IS_ERR(page));
	}

	lock_page(page);
	paddr = kmap_atomic(page);
	memcpy(paddr, req->page, PAGE_SIZE);
	kunmap_atomic(paddr);

	set_bit(my_nid, page->owners);
	clear_bit(req->remote_nid, page->owners);

	entry = pte_make_valid(*pte);

	set_pte_at(mm, addr, pte, entry);
	update_mmu_cache(vma, addr, pte);
	flush_tlb_page(vma, addr);

	unlock_page(page);
	put_page(page);

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
	BUG_ON(!page);

	if (test_bit(my_nid, page->owners) && pte_write(entry)) {
		void *paddr;
		printk("flush_remote_page [%d]:+ %lx\n", current->pid, addr);

		req->addr = addr;
		paddr = kmap_atomic(page);
		memcpy(req->page, paddr, PAGE_SIZE);
		kunmap_atomic(paddr);

		clear_bit(my_nid, page->owners);

		pcn_kmsg_send_long(current->origin_nid, req, sizeof(*req));
	} else {
		*pte = pte_make_valid(*pte);
		printk("flush_remote_page [%d]:- %lx\n", current->pid, addr);
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

	req->remote_nid = my_nid;
	req->remote_pid = current->pid;
	req->origin_pid = current->origin_pid;
	req->last = false;

	down_read(&mm->mmap_sem);
	for (vma = mm->mmap; vma; vma = vma->vm_next) {
		if (vma_is_anonymous(vma)) {
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
static int __do_invalidate_page(struct task_struct *tsk, unsigned long addr)
{
	struct mm_struct *mm = get_task_mm(tsk);
	struct page *page;
	struct vm_area_struct *vma;
	pte_t *pte, entry;
	spinlock_t *ptl;
	int ret = 0;

	down_read(&mm->mmap_sem);
	vma = find_vma(mm, addr);
	if (!vma || vma->vm_start > addr) {
		ret = VM_FAULT_SIGBUS;
		goto out;
	}

	page = __find_page_at(mm, addr, &pte, &ptl);
	if (IS_ERR(page)) {
		printk("invalidate_page [%d]: not exist at %lx\n", tsk->pid,  addr);
		ret = VM_FAULT_SIGBUS;
		goto out;
	}

	printk("invalidate_page [%d]: %lx\n", tsk->pid, addr);

	lock_page(page);
	clear_bit(my_nid, page->owners);

	entry = ptep_get_and_clear(mm, addr, pte);
	entry = pte_make_invalid(entry);

	set_pte_at(mm, addr, pte, entry);
	update_mmu_cache(vma, addr, pte);
	flush_tlb_page(vma, addr);

	unlock_page(page);
	put_page(page);

	pte_unmap_unlock(pte, ptl);

out:
	up_read(&mm->mmap_sem);
	mmput(mm);
	return 0;
}

static void process_remote_page_invalidate(struct work_struct *_work)
{
	struct pcn_kmsg_work *w = (struct pcn_kmsg_work *)_work;
	remote_page_invalidate_t *req = w->msg;
	struct task_struct *tsk;
	struct remote_context *rc;
	struct remote_page *rp;

	/* Only home issues invalidate requests. Hence, I am a remote */
	tsk = __get_task_struct(req->remote_pid);
	if (!tsk) {
		printk("%s: no such process %d %d\n", __func__,
				req->origin_pid, req->remote_pid);
		goto out_free;
	}

	rc = get_task_remote(tsk);
	do {
		unsigned long flags;

		spin_lock_irqsave(&rc->pages_lock, flags);
		rp = __lookup_pending_remote_page_request(rc, req->addr);
		spin_unlock_irqrestore(&rc->pages_lock, flags);
		if (rp) {
			printk("%s: MAPPING IN-PROGRESS\n", __func__);
			io_schedule();
		}
	} while (rp);

	__do_invalidate_page(tsk, req->addr);

	put_task_struct(tsk);
	put_task_remote(tsk);

out_free:
	pcn_kmsg_free_msg(req);
	kfree(w);
}


static void __revoke_page_ownership(struct task_struct *tsk, unsigned long addr, struct page *page, pte_t *pte, int except)
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
		if (nid == except) continue;

		clear_bit(nid, page->owners);

		if (nid == my_nid) continue;

		req.remote_pid = rc->remote_tgids[nid];
		printk("revoke_ownership [%d]: 0x%lx at %d %d\n", current->pid,
				addr, req.remote_pid, nid);
		pcn_kmsg_send_long(nid, &req, sizeof(req));
	}
	put_task_remote(tsk);
}



/**************************************************************************
 * Handle remote page fetch response
 */

static void process_remote_page_response(struct work_struct *_work)
{
	struct pcn_kmsg_work *w = (struct pcn_kmsg_work *)_work;
	remote_page_response_t *res = w->msg;
	struct task_struct *tsk;
	unsigned long flags;
	struct remote_page *rp;
	struct remote_context *rc;

	tsk = __get_task_struct(res->remote_pid);
	if (!tsk) {
		__WARN();
		goto out_free;
	}
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
	smp_mb();

	wake_up(&rp->pendings_wait);

out_free:
	kfree(w);
}


/**************************************************************************
 * Handle for remote page fetch
 */
static int __forward_remote_page_request(struct task_struct *tsk, remote_page_request_t *req, struct page *page)
{
	struct remote_context *rc = get_task_remote(tsk);
	int nid;
	bool forwarded = false;

	for_each_set_bit(nid, page->owners, MAX_POPCORN_NODES) {
		printk("forward_rp_request [%d]: %lx to %d at %d\n",
				tsk->pid, req->addr, rc->remote_tgids[nid], nid);
		req->origin_pid = rc->remote_tgids[nid];
		pcn_kmsg_send_long(nid, req, sizeof(*req));
		forwarded = true;
		break;
	}

	WARN_ON(!forwarded && "no page owner");

	put_task_remote(tsk);
	return forwarded ? VM_FAULT_FORWARDED : VM_FAULT_SIGBUS;
}


/* Defined in mm/memory.c */
int handle_pte_fault_origin(struct mm_struct *mm,
		struct vm_area_struct *vma, unsigned long address,
		pte_t *pte, pmd_t *pmd, unsigned int flags);


static pte_t *__get_pte_at(struct mm_struct *mm, struct vm_area_struct *vma, unsigned long addr, pmd_t **ppmd)
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
	*ppmd = pmd;

	pte = pte_alloc_map(mm, vma, pmd, addr);

	return pte;
}

static int __get_remote_page(struct task_struct *tsk,
		struct mm_struct *mm, struct vm_area_struct *vma,
		remote_page_request_t *req, remote_page_response_t *res)
{
	int ret = 0;
	int from = req->remote_nid;
	struct page *page;
	void *paddr;
	unsigned long addr = req->addr;
	pte_t entry;
	spinlock_t *ptl;

	pmd_t *pmd;
	pte_t *pte;

	/*
	 * We cannot simply do pte_alloc_map_lock() here since we might fallback
	 * to the origin pte fault handler and in which the pte_lock is acquired.
	 */
	pte = __get_pte_at(mm, vma, addr, &pmd);
	if (!pte) return VM_FAULT_OOM;

	if (pte_none(*pte)) {
		if (!tsk->at_remote) {
			ret = handle_pte_fault_origin(
					mm, vma, addr, pte, pmd, req->fault_flags);
			BUG_ON(ret);

			/* PTE is unmapped upon pte_fault return. Thus, re-map. */
			pte = pte_offset_map(pmd, addr);
			printk("remote_page_request [%d]: handled an origin fault!!\n",
					req->origin_pid);
		} else {
			return VM_FAULT_OOM;
		}
	}

	ptl = pte_lockptr(mm, pmd);
	spin_lock(ptl);

	page = vm_normal_page(vma, addr, *pte);
	BUG_ON(!page);

	get_page(page);
	lock_page(page);
	if (!page_is_mine(page)) {
		ret = __forward_remote_page_request(tsk, req, page);
		goto out_put;
	}

	paddr = kmap_atomic(page);
	memcpy(res->page, paddr, PAGE_SIZE);
	kunmap_atomic(paddr);

	set_bit(my_nid, page->owners);
	set_bit(from, page->owners);
	SetPageUptodate(page);

	if (!(req->fault_flags & FAULT_FLAG_WRITE)) {
		entry = pte_wrprotect(*pte);

		set_pte_at(mm, addr, pte, entry);
		update_mmu_cache(vma, addr, pte);
		flush_tlb_page(vma, addr);
	} else if (!(vma->vm_flags & VM_SHARED)) {
		entry = pte_make_invalid(*pte);

		set_pte_at(mm, addr, pte, entry);
		update_mmu_cache(vma, addr, pte);
		flush_tlb_page(vma, addr);

		__revoke_page_ownership(tsk, addr, page, pte, from);
	} else {
		BUG_ON("remote_page_request: shared fault. flush to file???");
	}

out_put:
	unlock_page(page);
	put_page(page);
	pte_unmap_unlock(pte, ptl);
	return ret;
}


static void __reply_remote_page(int nid, int remote_pid, remote_page_response_t *res)
{
	if (res->result == VM_FAULT_FORWARDED) return;

	res->header.type = PCN_KMSG_TYPE_REMOTE_PAGE_RESPONSE;
	res->header.prio = PCN_KMSG_PRIO_NORMAL;

	res->remote_pid = remote_pid;

	pcn_kmsg_send_long(nid, res, sizeof(*res));
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
	struct remote_context *rc;
	struct remote_page *rp;

	might_sleep();

	while (!res) {	/* response contains a page. allocate from a heap */
		res = kmalloc(sizeof(*res), GFP_KERNEL);
	}
	res->addr = req->addr;
	printk("\nremote_page_request [%d]: %lx %s from [%d/%d]\n",
			req->origin_pid, req->addr,
			req->fault_flags & FAULT_FLAG_WRITE ? "write" : "read",
			req->remote_pid, from);

	tsk = __get_task_struct(req->origin_pid);
	if (!tsk) {
		res->result = VM_FAULT_SIGBUS;
		printk("remote_page_request [%d]: not found\n", req->origin_pid);
		goto out;
	}
	mm = get_task_mm(tsk);

	down_read(&mm->mmap_sem);
	vma = find_vma(mm, req->addr);
	if (!vma || vma->vm_start > req->addr) {
		res->result = VM_FAULT_SIGBUS;
		goto out_up;
	}

	res->result = __get_remote_page(tsk, mm, vma, req, res);

out_up:
	up_read(&mm->mmap_sem);
	mmput(mm);
	put_task_struct(tsk);

out:
	if (res->result != VM_FAULT_FORWARDED) {
		__reply_remote_page(from, req->remote_pid, res);
	}
	pcn_kmsg_free_msg(req);
	kfree(w);
	kfree(res);
}


/**************************************************************************
 * Page fault handler
 */
static struct remote_page *__fetch_remote_page(struct remote_context *rc, unsigned long addr, unsigned long fault_flags)
{
	struct task_struct *tsk = current;
	struct remote_page *rp;
	unsigned long flags;
	DEFINE_WAIT(wait);
	remote_page_request_t req = {
		.addr = 0,	/* indicates not to send this request */
	};

	// TODO: deal with the do_fault_around

	spin_lock_irqsave(&rc->pages_lock, flags);
	rp = __lookup_pending_remote_page_request(rc, addr);
	if (!rp) {
		struct remote_page *r;
		spin_unlock_irqrestore(&rc->pages_lock, flags);

		rp = __alloc_remote_page_request(tsk, rc, addr, fault_flags, &req);

		spin_lock_irqsave(&rc->pages_lock, flags);
		r = __lookup_pending_remote_page_request(rc, addr);
		if (!r) {
			printk("%s [%d]: %lx [%d/%d]\n", __func__,
					tsk->pid, addr, req.origin_pid, rp->peer_nid);
			list_add(&rp->list, &rc->pages);
		} else {
			printk("%s [%d]: %lx pended\n", __func__, tsk->pid, addr);
			kfree(rp);
			rp = r;
			req.addr = 0;
		}
	} else {
		printk("%s [%d]: %lx pended\n", __func__, tsk->pid, addr);
	}
	atomic_inc(&rp->pendings);
	prepare_to_wait(&rp->pendings_wait, &wait, TASK_UNINTERRUPTIBLE);
	spin_unlock_irqrestore(&rc->pages_lock, flags);

	if (req.addr) {
		pcn_kmsg_send_long(rp->peer_nid, &req, sizeof(req));
	}

	io_schedule();

	/* Now the remote page would be brought in to rp */

	finish_wait(&rp->pendings_wait, &wait);

	return rp;
}


static int __map_remote_page(struct remote_page *rp,
		struct mm_struct *mm, struct vm_area_struct *vma,
		pmd_t *pmd, pte_t pte_val, unsigned long fault_flags)
{
	remote_page_response_t *res = rp->response;
	unsigned long addr = res->addr;
	struct page *page;
	void *paddr;
	pte_t *pte;
	spinlock_t *ptl = NULL;
	struct mem_cgroup *memcg;
	bool populated = false;
	int ret = 0;

	/* Something went wrong. Do not map this page */
	if (res->result) return res->result;

	if (anon_vma_prepare(vma)) {
		BUG_ON("Cannot prepare vma for anonymous page");
		return VM_FAULT_SIGBUS;
	}

	pte = pte_offset_map_lock(mm, pmd, addr, &ptl);
	if (!pte_same(pte_val, *pte)) {
		printk("%s [%d]: PTE CHANGED. CONTINUE\n", __func__, current->pid);
		pte_unmap_unlock(pte, ptl);
		return VM_FAULT_CONTINUE;
	}
	if (pte_none(*pte) || !(page = vm_normal_page(vma, addr, *pte))) {
		page = alloc_page_vma(GFP_HIGHUSER_MOVABLE, vma, addr);
		BUG_ON(!page);
		populated = true;
	}
	get_page(page);
	printk("%s [%d]: %p\n", __func__, current->pid, page);

	if (populated) {
		if (mem_cgroup_try_charge(page, mm, GFP_KERNEL, &memcg)) {
			ret = VM_FAULT_OOM;
			goto out;
		}
	}
	printk("%s [%d]: %s\n", __func__,
			current->pid, populated ? "populated" : "existing");

	lock_page(page);
	paddr = kmap_atomic(page);
	memcpy(paddr, res->page, PAGE_SIZE);
	kunmap_atomic(paddr);

	/* Track only my ownership for now to simplify the protocol */
	set_bit(my_nid, page->owners);

	SetPageUptodate(page);

	if (populated) {
		do_set_pte(vma, addr, page, pte, fault_flags & FAULT_FLAG_WRITE, true);

		mem_cgroup_commit_charge(page, memcg, false);
		lru_cache_add_active_or_unevictable(page, vma);
	} else {
		pte_t entry = pte_make_valid(*pte);

		if (fault_flags & FAULT_FLAG_WRITE) {
			entry = pte_mkwrite(entry);
			entry = pte_mkdirty(entry);
		} else {
			entry = pte_wrprotect(entry);
		}
		set_pte_at(mm, addr, pte, entry);
		update_mmu_cache(vma, addr, pte);
		flush_tlb_page(vma, addr);
	}
	unlock_page(page);

out:
	put_page(page);
	pte_unmap_unlock(pte, ptl);
	return ret;
}


static int __handle_remote_fault(struct mm_struct *mm,
		struct vm_area_struct *vma, unsigned long addr,
		pmd_t *pmd, pte_t *pte, pte_t pte_val, unsigned long fault_flags)
{
	struct task_struct *tsk = current;
	struct remote_context *rc = get_task_remote(tsk);
	struct remote_page *rp;
	unsigned long flags;
	bool wakeup = false;
	int remaining = 0;
	int ret = 0;

	pte_unmap(pte);

	rp = __fetch_remote_page(rc, addr, fault_flags);

	if (!rp->mapped) {
		rp->ret = __map_remote_page(rp, mm, vma, pmd, pte_val, fault_flags);
		rp->mapped = true;
	}
	ret = rp->ret;

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
	smp_mb();

	printk("__done_remote_fault [%d]: %lx %d / %d\n",
			tsk->pid, addr, ret, remaining);

	if (wakeup) wake_up(&rp->pendings_wait);

	return ret;
}


static int __handle_write_at_origin(struct mm_struct *mm,
		struct vm_area_struct *vma, unsigned long addr,
		pmd_t *pmd, pte_t *pte, pte_t pte_val, struct page *page)
{
	spinlock_t *ptl;
	struct page *page;

	ptl = pte_lockptr(mm, pmd);
	spin_lock(ptl);
	if (!pte_same(*pte, pte_val)) {
		goto out;
	}

	pte_val = pte_mkwrite(*pte);
	set_pte_at(mm, addr, pte, pte_val);
	update_mmu_cache(vma, addr, pte);
	flush_tlb_page(vma, addr);

	__revoke_page_ownership(current, addr, page, pte, my_nid);

out:
	pte_unmap_unlock(pte, ptl);
	//printk("WRITE_AT_ORIGIN: %lx\n", addr);
	return 0;
}


static int __handle_fault_at_origin(struct mm_struct *mm,
		struct vm_area_struct *vma, unsigned long addr,
		pmd_t *pmd, pte_t *pte, pte_t pte_val, unsigned int fault_flags)
{
	struct page *page;
	spinlock_t *ptl;

	/* Fresh access to the address. Handle locally since we are at the origin */
	if (pte_none(pte_val)) {
		BUG_ON(pte_present(pte_val));

		printk("pte_fault [%d]: fresh at origin. continue\n", current->pid);
		return VM_FAULT_CONTINUE;
	}

	page = vm_normal_page(vma, addr, pte_val);
	BUG_ON(!page);

	if (page_is_replicated(page)) {
		/* Handle replicated page via the dsm protocol */
		printk("pte_fault [%d]: handle replicated page at the origin\n",
				current->pid);
		if (page_is_mine(page)) {
			BUG_ON(!(fault_flags & FAULT_FLAG_WRITE));
			return __handle_write_at_origin(
					mm, vma, addr, pmd, pte, pte_val, page);
		}
		return __handle_remote_fault(
				mm, vma, addr, pmd, pte, pte_val, fault_flags);
	}

	/* Nothing to do with the DSM protocol (e.g., COW). Handle locally */
	printk("pte_fault [%d]: local fault at origin. continue\n", current->pid);
	return VM_FAULT_CONTINUE;
}


/**
 * Function:
 *	page_server_handle_pte_fault
 *
 * Description:
 *	Handle PTE faults with Popcorn page replication protocol.
 *  down_read(&mm->mmap_sem) is already held when getting in.
 *  DO NOT FORGET to unmap pte before return non-VM_FAULT_CONTINUE.
 *
 * Input:
 *	All are from the PTE handler
 *
 * Return values:
 *	VM_FAULT_CONTINUE when the page fault can be handled locally.
 *	0 if the fault is fetched remotely and fixed.
 */
int page_server_handle_pte_fault(
		struct mm_struct *mm, struct vm_area_struct *vma,
		unsigned long address, pmd_t *pmd, pte_t *pte, pte_t pte_val,
		unsigned int fault_flags)
{
	unsigned long addr = address & PAGE_MASK;

	might_sleep();

	printk(KERN_WARNING"\n## PAGEFAULT [%d]: %lx %c %lx %x %lx\n",
			current->pid, address,
			fault_flags & FAULT_FLAG_WRITE ? 'W' : 'R',
			instruction_pointer(current_pt_regs()),
			fault_flags, pte_val(pte_val));

	/*
	 * Distributed process but local thread
	 */
	if (!current->at_remote) {
		return __handle_fault_at_origin(
				mm, vma, addr, pmd, pte, pte_val, fault_flags);
	}

	/*
	 * Distributed process, distributed thread
	 *
	 * Fault handling at the remote side is simpler than at the origin.
	 * There will be no copy-on-write case at the remote since no thread
	 * creation is allowed at the remote side.
	 */
	if (pte_none(pte_val)) {
		/* Can we handle the fault locally? */
		if (vma->vm_flags & VM_FETCH_LOCAL) {
			printk("pte_fault [%d]: VM_FETCH_LOCAL. continue\n", current->pid);
			return VM_FAULT_CONTINUE;
		}
		if (!vma_is_anonymous(vma) &&
				((vma->vm_flags & (VM_WRITE | VM_SHARED)) == 0)) {
			printk("pte_fault [%d]: locally file-mapped read-only. continue\n",
					current->pid);
			return VM_FAULT_CONTINUE;
		}
	}

	/* pte exists at this point. Let's bring the DSM protocol! */

	printk("pte_fault [%d]: handle %s\n", current->pid,
			fault_flags & FAULT_FLAG_WRITE ? "write" : "read");
	return __handle_remote_fault(mm, vma, addr, pmd, pte, pte_val, fault_flags);
}


/**************************************************************************
 * Routing popcorn messages to worker
 */

DEFINE_KMSG_WQ_HANDLER(remote_page_request);
DEFINE_KMSG_WQ_HANDLER(remote_page_response);
DEFINE_KMSG_WQ_HANDLER(remote_page_invalidate);
DEFINE_KMSG_ORDERED_WQ_HANDLER(remote_page_flush);

int __init page_server_init(void)
{
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
