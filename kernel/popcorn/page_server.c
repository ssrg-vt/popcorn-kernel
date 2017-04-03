/**
 * @file page_server.c
 *
 * Popcorn Linux page server implementation
 * This work was an extension of Marina Sadini MS Thesis, but totally revamped
 * for multiple node setup.
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

bool fault_for_write(unsigned long flags)
{
	return !!(flags & FAULT_FLAG_WRITE);
}

bool fault_for_read(unsigned long flags)
{
	return !fault_for_write(flags);
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

	// Lock
	page = __find_page_at(mm, addr, &pte, &ptl);
	if (IS_ERR(page)) {
		printk("\nINVALIDATE_PAGE [%d]: not exist at %lx\n", tsk->pid,  addr);
		ret = VM_FAULT_SIGBUS;
		goto out;
	}

	printk("\nINVALIDATE_PAGE [%d]: %lx\n", tsk->pid, addr);

	clear_bit(my_nid, page->owners);

	entry = pte_make_invalid(*pte);

	set_pte_at(mm, addr, pte, entry);
	update_mmu_cache(vma, addr, pte);
	flush_tlb_page(vma, addr);

	put_page(page);
	pte_unmap_unlock(pte, ptl);

out:
	up_read(&mm->mmap_sem);
	mmput(mm);

	return 0;
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
		.remote_pid = req->remote_pid,
	};
	struct task_struct *tsk;

	/* Only home issues invalidate requests. Hence, I am a remote */
	tsk = __get_task_struct(req->remote_pid);
	if (!tsk) {
		printk("%s: no such process %d %d %lx\n", __func__,
				req->origin_pid, req->remote_pid, req->addr);
		goto out_free;
	}

	__do_invalidate_page(tsk, req->addr);

	pcn_kmsg_send_long(req->origin_nid, &res, sizeof(res));

	put_task_struct(tsk);

out_free:
	pcn_kmsg_free_msg(req);
	kfree(w);
}


static void process_page_invalidate_response(struct work_struct *work)
{
	struct pcn_kmsg_work *w = (struct pcn_kmsg_work *)work;
	page_invalidate_response_t *res = w->msg;
	struct task_struct *tsk;

	tsk = __get_task_struct(res->origin_pid);
	if (!tsk) {
		__WARN();
		goto out;
	}
	if (!atomic_dec_return(&tsk->pendings_count)) {
		complete(&tsk->pendings);
	}

	put_task_struct(tsk);
out:
	pcn_kmsg_free_msg(res);
	kfree(w);
}


static void __revoke_page_ownership(struct task_struct *tsk, unsigned long addr, struct page *page, pte_t *pte, int except)
{
	int nid;
	struct remote_context *rc = get_task_remote(tsk);
	page_invalidate_request_t req = {
		.header.type = PCN_KMSG_TYPE_PAGE_INVALIDATE_REQUEST,
		.header.prio = PCN_KMSG_PRIO_NORMAL,
		.addr = addr,
		.origin_nid = my_nid,
		.origin_pid = tsk->pid,
	};
	int peers = bitmap_weight(page->owners, MAX_POPCORN_NODES);

	if (except >= 0 && test_bit(except, page->owners)) peers--;
	if (test_bit(my_nid, page->owners)) peers--;
	atomic_set(&tsk->pendings_count, peers);

	/* Only the origin broadcasts the revocation message. */
	for_each_set_bit(nid, page->owners, MAX_POPCORN_NODES) {
		if (nid == except) continue;

		clear_bit(nid, page->owners);

		if (nid == my_nid) continue;

		req.remote_pid = rc->remote_tgids[nid];
		printk("revoke_ownership [%d]: 0x%lx at [%d/%d]\n",
				current->pid, addr, req.remote_pid, nid);
		pcn_kmsg_send_long(nid, &req, sizeof(req));
	}

	// Put this task into wait until peers invalidate the page
	if (peers > 0) {
		wait_for_completion(&tsk->pendings);
		printk("revoke_ownership [%d]: completed %d\n", current->pid, peers);
	}

	put_task_remote(tsk);
}


/**************************************************************************
 * Handle page faults happened at remote nodes.
 */

static void process_remote_page_response(struct work_struct *work)
{
	struct pcn_kmsg_work *w = (struct pcn_kmsg_work *)work;
	remote_page_response_t *res = w->msg;
	struct task_struct *tsk;

	tsk = __get_task_struct(res->remote_pid);
	if (!tsk) {
		__WARN();
		goto out_free;
	}
	tsk->private = res;
	smp_mb();

	complete(&tsk->pendings);
	put_task_struct(tsk);

out_free:
	kfree(w);
}


static remote_page_response_t *__fetch_remote_page(int from_nid, pid_t from_pid, unsigned long addr, unsigned long fault_flags)
{
	remote_page_request_t req = {
		.header = {
			.type = PCN_KMSG_TYPE_REMOTE_PAGE_REQUEST,
			.prio = PCN_KMSG_PRIO_NORMAL,
		},

		.addr = addr,
		.fault_flags = fault_flags,

		.remote_nid = my_nid,
		.remote_pid = current->pid,

		.origin_pid = from_pid,
	};

	pcn_kmsg_send_long(from_nid, &req, sizeof(req));

	printk("  [%d]: ->[%d/%d]\n", current->pid, from_pid, from_nid);
	wait_for_completion(&current->pendings);
	printk("  [%d]: <-[%d/%d]\n", current->pid, from_pid, from_nid);

	/* Now the remote page would be brought in to task_struct->private */

	return current->private;
}


static int __map_remote_page(remote_page_response_t *res,
		struct mm_struct *mm, struct vm_area_struct *vma,
		pmd_t *pmd, pte_t *pte, unsigned long fault_flags)
{
	unsigned long addr = res->addr;
	struct page *page;
	void *paddr;
	struct mem_cgroup *memcg;
	bool populated = false;
	int ret = 0;

	/* Something went wrong. Do not map this page */
	if (res->result) return res->result;

	if (anon_vma_prepare(vma)) {
		BUG_ON("Cannot prepare vma for anonymous page");
		return VM_FAULT_SIGBUS;
	}

	if (pte_none(*pte) || !(page = vm_normal_page(vma, addr, *pte))) {
		page = alloc_page_vma(GFP_HIGHUSER_MOVABLE, vma, addr);
		BUG_ON(!page);
		populated = true;
	}

	get_page(page);
	//printk("%s [%d]: %p\n", __func__, current->pid, page);

	if (populated) {
		if (mem_cgroup_try_charge(page, mm, GFP_KERNEL, &memcg)) {
			ret = VM_FAULT_OOM;
			goto out;
		}
	}
	printk("%s [%d]: %s\n", __func__,
			current->pid, populated ? "populated" : "existing");

	flush_icache_page(vma, page);
	lock_page(page);
	paddr = kmap_atomic(page);
	memcpy(paddr, res->page, PAGE_SIZE);
	kunmap_atomic(paddr);

	SetPageUptodate(page);
	unlock_page(page);

	if (populated) {
		do_set_pte(vma, addr, page, pte, fault_for_write(fault_flags), true);

		mem_cgroup_commit_charge(page, memcg, false);
		lru_cache_add_active_or_unevictable(page, vma);
	} else {
		pte_t entry = pte_make_valid(*pte);

		if (fault_for_write(fault_flags)) {
			entry = pte_mkwrite(entry);
			entry = pte_mkdirty(entry);
		} else {
			entry = pte_wrprotect(entry);
		}
		set_pte_at(mm, addr, pte, entry);
		update_mmu_cache(vma, addr, pte);
		flush_tlb_page(vma, addr);
	}
	set_bit(my_nid, page->owners);

out:
	put_page(page);
	return ret;
}

static int __handle_remote_page(int nid, pid_t pid,
		struct mm_struct *mm, struct vm_area_struct *vma, unsigned long addr,
		pmd_t *pmd, pte_t *pte, unsigned long fault_flags)
{
	remote_page_response_t *res;
	int ret = 0;

	res = __fetch_remote_page(nid, pid, addr, fault_flags);

	ret = __map_remote_page(res, mm, vma, pmd, pte, fault_flags);

	printk("__done_remote_fault [%d]: %lx %d\n", current->pid, addr, ret);

	if (ret == VM_FAULT_RETRY) ret = 0;

	pcn_kmsg_free_msg(res);

	return ret;
}



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

static int __handle_remotefault_at_remote(struct task_struct *tsk, struct mm_struct *mm, struct vm_area_struct *vma, remote_page_request_t *req, remote_page_response_t *res)
{
	unsigned long addr = req->addr;
	unsigned char *paddr;
	struct page *page;

	spinlock_t *ptl;
	pmd_t *pmd;
	pte_t *pte;
	pte_t entry;

	pte = __get_pte_at(mm, vma, addr, &pmd);
	if (!pte) return VM_FAULT_OOM;
	if (pte_none(*pte)) {
		pte_unmap(pte);
		return VM_FAULT_SIGSEGV;
	}

	// Might not content as much as at the origin since it only communicate
	// with the origin.
	ptl = pte_lockptr(mm, pmd);
	spin_lock(ptl);

	page = vm_normal_page(vma, addr, *pte);
	BUG_ON(!page);

	BUG_ON(!page_is_mine(page));

	if (fault_for_write(req->fault_flags)) {
		// drop my ownership
		entry = pte_make_invalid(*pte);
		clear_bit(my_nid, page->owners);
	} else {
		entry = pte_wrprotect(*pte);
	}

	set_pte_at(mm, addr, pte, entry);
	update_mmu_cache(vma, addr, pte);
	flush_tlb_page(vma, addr);

	lock_page(page);
	paddr = kmap_atomic(page);
	memcpy(res->page, paddr, PAGE_SIZE);
	kunmap_atomic(paddr);
	unlock_page(page);

	pte_unmap_unlock(pte, ptl);

	return 0;
}


int handle_pte_fault_origin(struct mm_struct *mm,
		struct vm_area_struct *vma, unsigned long address,
		pte_t *pte, pmd_t *pmd, unsigned int flags);

static int __handle_remotefault_at_origin(struct task_struct *tsk, struct mm_struct *mm, struct vm_area_struct *vma, remote_page_request_t *req, remote_page_response_t *res)
{
	int from = req->remote_nid;
	unsigned long addr = req->addr;
	unsigned char *paddr;
	struct page *page;
	int ret;

	spinlock_t *ptl;
	pmd_t *pmd;
	pte_t *pte;
	pte_t entry;

again:
	pte = __get_pte_at(mm, vma, addr, &pmd);
	if (!pte) {
		printk("  [%d]: no pte\n", tsk->pid);
		return VM_FAULT_OOM;
	}

	if (pte_none(*pte)) {
		printk("  [%d]: handle local fault at origin\n", tsk->pid);

		ret = handle_pte_fault_origin(
				mm, vma, addr, pte, pmd, req->fault_flags);
		if (ret) {
			return ret;
		}

		goto again;
	}

	ptl = pte_lockptr(mm, pmd);
	if (!spin_trylock(ptl)) {
		pte_unmap(pte);
		return VM_FAULT_RETRY;
	}

	page = vm_normal_page(vma, addr, *pte);
	BUG_ON(!page);

	if (!page_is_mine(page)) {
		/*
		// Recursive fault: This fault cannot be handled at the origin
		// for now. Bring the page from one of owners, maps to me, and
		// send the results
		ret = __handle_remote_page(mm, vma, pmd, pte, ptl, fault_flags, nid);
		*/
	}
	set_bit(my_nid, page->owners);

	if (fault_for_write(req->fault_flags)) {
		__revoke_page_ownership(tsk, addr, page, pte, from);
		entry = pte_make_invalid(*pte);
	} else {
		entry = pte_wrprotect(*pte);
	}
	set_bit(from, page->owners);

	set_pte_at(mm, addr, pte, entry);
	update_mmu_cache(vma, addr, pte);
	flush_tlb_page(vma, addr);

	lock_page(page);
	paddr = kmap_atomic(page);
	memcpy(res->page, paddr, PAGE_SIZE);
	kunmap_atomic(paddr);
	unlock_page(page);

	pte_unmap_unlock(pte, ptl);
	return 0;
}


/**
 * Entry point to handle remote page faults
 */
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
	}
	res->addr = req->addr;
	printk("\nREMOTE_PAGE_REQUEST [%d]: %lx %s from [%d/%d]\n",
			req->origin_pid, req->addr,
			fault_for_write(req->fault_flags) ? "W" : "R",
			req->remote_pid, from);

	tsk = __get_task_struct(req->origin_pid);
	if (!tsk) {
		res->result = VM_FAULT_SIGBUS;
		printk("  [%d]: not found\n", req->origin_pid);
		goto out;
	}
	mm = get_task_mm(tsk);

	down_read(&mm->mmap_sem);
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
	up_read(&mm->mmap_sem);
	mmput(mm);
	put_task_struct(tsk);

out:
	res->header.type = PCN_KMSG_TYPE_REMOTE_PAGE_RESPONSE;
	res->header.prio = PCN_KMSG_PRIO_NORMAL;

	res->remote_pid = req->remote_pid;
	printk("  [%d]: %lx %d\n", req->origin_pid, req->addr, res->result);

	pcn_kmsg_send_long(from, res, sizeof(*res));

	kfree(res);

	pcn_kmsg_free_msg(req);
	kfree(w);
}



/**************************************************************************
 * Handle page faults occured at the current node. The fault could be
 * handled at the current node or at the remote node.
 * To prevent concurrent PTE updates, faults might not be handled
 * immediately, but handled by causing the fault again.
 *
 * TODO: deal with the do_fault_around
 */

static int __handle_localfault_at_remote(struct mm_struct *mm,
		struct vm_area_struct *vma, unsigned long addr,
		pmd_t *pmd, pte_t *pte, pte_t pte_val, unsigned int fault_flags)
{
	int ret;
	spinlock_t *ptl = pte_lockptr(mm, pmd);

	if (!spin_trylock(ptl)) {
		pte_unmap(pte);
		printk("  [%d]: someone already handling this. retry\n", current->pid);
		msleep(100);
		io_schedule();
		return 0;
	}
	if (!pte_same(*pte, pte_val)) {
		pte_unmap_unlock(pte, ptl);
		printk("  [%d]: someone already handled this. cool\n", current->pid);
		return 0;
	}

	ret = __handle_remote_page(current->origin_nid, current->origin_pid,
			mm, vma, addr, pmd, pte, fault_flags);

	pte_unmap_unlock(pte, ptl);
	return ret;
}


static int __handle_localfault_at_origin(struct mm_struct *mm,
		struct vm_area_struct *vma, unsigned long addr,
		pmd_t *pmd, pte_t *pte, pte_t pte_val, unsigned int fault_flags)
{
	struct page *page;
	spinlock_t *ptl;

	/* Fresh access to the address. Handle locally since we are at the origin */
	if (pte_none(pte_val)) {
		BUG_ON(pte_present(pte_val));

		printk("  [%d]: fresh at origin. continue\n", current->pid);
		return VM_FAULT_CONTINUE;
	}

	ptl = pte_lockptr(mm, pmd);
	if (!spin_trylock(ptl)) {
		pte_unmap(pte);
		printk("  [%d]: someone already handling this. retry\n", current->pid);
		msleep(100);
		io_schedule();
		return 0;
	}
	if (!pte_same(*pte, pte_val)) {
		pte_unmap_unlock(pte, ptl);
		printk("  [%d]: someone already handled this. cool!\n", current->pid);
		return 0;
	}

	page = vm_normal_page(vma, addr, pte_val);
	if (page == NULL ) {
		printk("  [%d]: No page??????\n", current->pid);
		goto out_continue;
	}

	if (page_is_replicated(page)) {
		/* Handle replicated page via the dsm protocol */
		printk("  [%d]: handle replicated page at the origin. %p %s\n",
				current->pid, ptl,
				page_is_mine(page) ? "mine" : "others");
		if (page_is_mine(page)) {
			if (fault_for_write(fault_flags)) {
				__revoke_page_ownership(current, addr, page, pte, -1);

				pte_val = pte_mkwrite(pte_val);
				pte_val = pte_mkdirty(pte_val);
			} else {
				BUG_ON("READ fault for owner??");
			}
			pte_val = pte_make_valid(pte_val);

			set_pte_at(mm, addr, pte, pte_val);
			update_mmu_cache(vma, addr, pte);
			flush_tlb_page(vma, addr);

			pte_unmap_unlock(pte, ptl);
			return 0;
		} else {
			struct remote_context *rc = get_task_remote(current);
			int nid = find_first_bit(page->owners, MAX_POPCORN_NODES);
			int ret;
			BUG_ON(nid == my_nid || nid < 0 || nid >= MAX_POPCORN_NODES);

			ret = __handle_remote_page(nid, rc->remote_tgids[nid],
					mm, vma, addr, pmd, pte, fault_flags);

			pte_unmap_unlock(pte, ptl);
			put_task_remote(current);
			return ret;
		}
	}

out_continue:
	spin_unlock(ptl);

	/* Nothing to do with the DSM protocol (e.g., COW). Handle locally */
	printk("  [%d]: local at origin. continue\n", current->pid);
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
			fault_for_write(fault_flags) ? 'W' : 'R',
			instruction_pointer(current_pt_regs()),
			fault_flags, pte_val(pte_val));

	/*
	 * Distributed process but local thread
	 */
	if (!current->at_remote) {
		return __handle_localfault_at_origin(
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
			printk("  [%d]: VM_FETCH_LOCAL. continue\n", current->pid);
			return VM_FAULT_CONTINUE;
		}
		if (!vma_is_anonymous(vma) &&
				((vma->vm_flags & (VM_WRITE | VM_SHARED)) == 0)) {
			printk("  [%d]: locally file-mapped read-only. continue\n",
					current->pid);
			return VM_FAULT_CONTINUE;
		}
	}

	/* pte exists at this point. Let's bring the DSM protocol! */
	if (fault_flags & FAULT_FLAG_TRIED) {
		return VM_FAULT_CONTINUE;
	}

	printk("  [%d]: handle remote fault\n", current->pid);
	return __handle_localfault_at_remote(
			mm, vma, addr, pmd, pte, pte_val, fault_flags);
}


/**************************************************************************
 * Routing popcorn messages to worker
 */

DEFINE_KMSG_WQ_HANDLER(remote_page_request);
DEFINE_KMSG_WQ_HANDLER(remote_page_response);
DEFINE_KMSG_WQ_HANDLER(page_invalidate_request);
DEFINE_KMSG_WQ_HANDLER(page_invalidate_response);
DEFINE_KMSG_ORDERED_WQ_HANDLER(remote_page_flush);

int __init page_server_init(void)
{
	REGISTER_KMSG_WQ_HANDLER(
			PCN_KMSG_TYPE_REMOTE_PAGE_REQUEST, remote_page_request);
	REGISTER_KMSG_WQ_HANDLER(
			PCN_KMSG_TYPE_REMOTE_PAGE_RESPONSE, remote_page_response);
	REGISTER_KMSG_WQ_HANDLER(
			PCN_KMSG_TYPE_PAGE_INVALIDATE_REQUEST, page_invalidate_request);
	REGISTER_KMSG_WQ_HANDLER(
			PCN_KMSG_TYPE_PAGE_INVALIDATE_RESPONSE, page_invalidate_response);
	REGISTER_KMSG_WQ_HANDLER(
			PCN_KMSG_TYPE_REMOTE_PAGE_FLUSH, remote_page_flush);

	return 0;
}
