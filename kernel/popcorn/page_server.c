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
#include <linux/random.h>

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
#include "wait_station.h"

static inline bool page_is_replicated(struct page *page)
{
	return !!bitmap_weight(page->owners, MAX_POPCORN_NODES);
}

static inline bool page_is_mine(struct page *page)
{
	return !page_is_replicated(page) || test_bit(my_nid, page->owners);
}

static inline bool fault_for_write(unsigned long flags)
{
	return !!(flags & FAULT_FLAG_WRITE);
}

static inline bool fault_for_read(unsigned long flags)
{
	return !fault_for_write(flags);
}

/**
 * Fault tracking mechanism
 *
 */


enum {
	FAULT_HANDLE_START = 0x01,
	FAULT_HANDLE_RETRY = 0x02,
	FAULT_HANDLE_DENIED = 0x04,

	FAULT_HANDLE_LEADER = 0x10,

	FAULT_FLAG_REMOTE = 0x100,
};

struct fault_handle {
	struct list_head list;

	unsigned long addr;

	bool write;
	bool remote;

	unsigned int backoff;

	int pendings;
	wait_queue_head_t waits;
	struct remote_context *rc;
};


static bool __check_fault_handling(struct task_struct *tsk, unsigned long addr)
{
	unsigned long flags;
	bool found = false;
	struct remote_context *rc = get_task_remote(tsk);
	struct fault_handle *fh;

	spin_lock_irqsave(&rc->faults_lock, flags);
	list_for_each_entry(fh, &rc->faults, list) {
		if (fh->addr == addr) {
			found = true;
			break;
		}
	}
	spin_unlock_irqrestore(&rc->faults_lock, flags);
	put_task_remote(tsk);
	if (found) msleep(1);
	return found;
}


static bool __start_fault_handling(struct task_struct *tsk, unsigned long addr, unsigned long fault_flags, spinlock_t *ptl, wait_queue_t *wait, struct fault_handle **handle, bool *leader)
	__releases(ptl)
{
	unsigned long flags;
	struct fault_handle *fh;
	bool found = false;
	unsigned int backoff;
	struct remote_context *rc = get_task_remote(tsk);
	char *contented = NULL;

	spin_lock_irqsave(&rc->faults_lock, flags);
	spin_unlock(ptl);

	list_for_each_entry(fh, &rc->faults, list) {
		if (fh->addr == addr) {
			found = true;
			break;
		}
	}

	if (found) {
		if (fh->remote) {
			contented = "remote";
			goto out_unlock;
		}
		if (fault_flags & FAULT_FLAG_REMOTE) {
			contented = "local";
			goto out_unlock;
		}
		if (fh->write != fault_for_write(fault_flags)) {
			contented = fh->write ? "write" : "read";
			goto out_unlock;
		}
		fh->pendings++;
		prepare_to_wait(&fh->waits, wait, TASK_UNINTERRUPTIBLE);
		spin_unlock_irqrestore(&rc->faults_lock, flags);
		put_task_remote(tsk);

		io_schedule();

		finish_wait(&fh->waits, wait);

		*handle = fh;
		*leader = false;
		return true;
	}

	fh = kmalloc(sizeof(*fh), GFP_ATOMIC);

	INIT_LIST_HEAD(&fh->list);
	fh->addr = addr;

	fh->write = fault_for_write(fault_flags);
	fh->remote = !!(fault_flags & FAULT_FLAG_REMOTE);
	init_waitqueue_head(&fh->waits);
	fh->pendings = 1;
	fh->backoff = 0;
	fh->rc = rc;

	list_add(&fh->list, &rc->faults);
	spin_unlock_irqrestore(&rc->faults_lock, flags);

	*handle = fh;
	*leader = true;
	return true;

out_unlock:
	backoff = fh->backoff++;
	spin_unlock_irqrestore(&rc->faults_lock, flags);

	put_task_remote(tsk);
	printk("  [%d]: CONTENDED %s. BACK OFF %d\n", tsk->pid, contented, backoff);

	msleep(backoff);
	return false;
}


static void __continue_fault_handling(struct fault_handle *fh)
{
	unsigned long flags;
	bool last = false;

	spin_lock_irqsave(&fh->rc->faults_lock, flags);
	if (--fh->pendings > 0) {
		wake_up(&fh->waits);
	} else {
		list_del(&fh->list);
		last = true;
	}
	spin_unlock_irqrestore(&fh->rc->faults_lock, flags);

	if (last) {
		__put_task_remote(fh->rc);
		kfree(fh);
	}
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

static pte_t *__get_pte_at(struct mm_struct *mm, struct vm_area_struct *vma, unsigned long addr, pmd_t **ppmd, spinlock_t **ptlp)
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
	*ptlp = pte_lockptr(mm, pmd);

	pte = pte_alloc_map(mm, vma, pmd, addr);

	return pte;
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

	flush_icache_page(vma, page);
	lock_page(page);
	paddr = kmap_atomic(page);
	memcpy(paddr, req->page, PAGE_SIZE);
	kunmap_atomic(paddr);

	set_bit(my_nid, page->owners);
	clear_bit(req->remote_nid, page->owners);

	entry = pte_make_valid(*pte);

	set_pte_at_notify(mm, addr, pte, entry);
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

	if (test_bit(my_nid, page->owners)) {
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

static int __do_invalidate_page(struct task_struct *tsk, page_invalidate_request_t *req)
{
	struct mm_struct *mm = get_task_mm(tsk);
	struct page *page;
	struct vm_area_struct *vma;
	pmd_t *pmd;
	pte_t *pte, entry;
	spinlock_t *ptl;
	int ret = 0;
	unsigned long addr = req->addr;

	down_read(&mm->mmap_sem);
	vma = find_vma(mm, addr);
	if (!vma || vma->vm_start > addr) {
		ret = VM_FAULT_SIGBUS;
		goto out;
	}

	printk("\nINVALIDATE_PAGE [%d]: %lx [%d/%d]\n", tsk->pid, addr,
			req->origin_pid, req->origin_nid);

	pte = __get_pte_at(mm, vma, addr, &pmd, &ptl);
	do {
		spin_lock(ptl);
	} while (__check_fault_handling(tsk, addr));

	page = vm_normal_page(vma, addr, *pte);
	get_page(page);

	clear_bit(my_nid, page->owners);

	entry = pte_make_invalid(*pte);

	set_pte_at_notify(mm, addr, pte, entry);
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
		.origin_ws = req->origin_ws,
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

	__do_invalidate_page(tsk, req);

	pcn_kmsg_send_long(req->origin_nid, &res, sizeof(res));
	printk("  [%d]: ->[%d/%d]\n", req->remote_pid,
			res.origin_pid, res.origin_nid);

	put_task_struct(tsk);

out_free:
	pcn_kmsg_free_msg(req);
	kfree(w);
}


static void process_page_invalidate_response(struct work_struct *work)
{
	struct pcn_kmsg_work *w = (struct pcn_kmsg_work *)work;
	page_invalidate_response_t *res = w->msg;
	struct wait_station *ws = wait_station(res->origin_ws);

	if (atomic_dec_and_test(&ws->pendings_count)) {
		complete(&ws->pendings);
	}

	pcn_kmsg_free_msg(res);
	kfree(w);
}


static void __revoke_page_ownership(struct task_struct *tsk, int nid, pid_t pid, unsigned long addr, int ws_id)
{
	page_invalidate_request_t req = {
		.header.type = PCN_KMSG_TYPE_PAGE_INVALIDATE_REQUEST,
		.header.prio = PCN_KMSG_PRIO_NORMAL,
		.addr = addr,
		.origin_nid = my_nid,
		.origin_pid = tsk->pid,
		.origin_ws = ws_id,
		.remote_pid = pid,
	};

	printk("  [%d]: revoke 0x%lx from [%d/%d]\n", tsk->pid, addr, pid, nid);
	pcn_kmsg_send_long(nid, &req, sizeof(req));
}


/**************************************************************************
 * Handle page faults happened at remote nodes.
 */

static void process_remote_page_response(struct work_struct *work)
{
	struct pcn_kmsg_work *w = (struct pcn_kmsg_work *)work;
	remote_page_response_t *res = w->msg;
	struct wait_station *ws = wait_station(res->origin_ws);

	printk("  [%d]: <-[%d/%d] %lx\n",
			ws->pid, res->remote_pid, res->remote_nid, res->addr);
	ws->private = res;

	smp_mb();
	if (atomic_dec_and_test(&ws->pendings_count))
		complete(&ws->pendings);

	kfree(w);
}


static void __fetch_remote_page(struct task_struct *tsk, int from_nid, pid_t from_pid, unsigned long addr, unsigned long fault_flags, int ws_id)
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

	pcn_kmsg_send_long(from_nid, &req, sizeof(req));

	printk("  [%d]: ->[%d/%d] %lx\n", tsk->pid, from_pid, from_nid, addr);
}


static int __map_remote_page(struct mm_struct *mm,
		struct vm_area_struct *vma, unsigned long addr,
		unsigned long fault_flags, pte_t *pte,
		struct page *page, remote_page_response_t *remote_page)
{
	void *paddr;
	int ret = 0;
	pte_t entry;

	flush_icache_page(vma, page);
	lock_page(page);
	paddr = kmap_atomic(page);
	memcpy(paddr, remote_page->page, PAGE_SIZE);
	kunmap_atomic(paddr);

	SetPageUptodate(page);
	unlock_page(page);

	entry = pte_make_valid(*pte);

	if (fault_for_write(fault_flags)) {
		entry = pte_mkwrite(entry);
		entry = pte_mkdirty(entry);
	} else {
		entry = pte_wrprotect(entry);
	}
	set_pte_at_notify(mm, addr, pte, entry);
	update_mmu_cache(vma, addr, pte);
	flush_tlb_page(vma, addr);

	set_bit(my_nid, page->owners);

	return ret;
}

static void __claim_local_page(struct task_struct *tsk, unsigned long addr, struct page *page, int except_nid)
{
	int peers = bitmap_weight(page->owners, MAX_POPCORN_NODES) - 1;
	BUG_ON(!test_bit(except_nid, page->owners) || peers < 0);

	if (peers > 0) {
		int nid;
		struct remote_context *rc = get_task_remote(tsk);
		struct wait_station *ws = get_wait_station(tsk->pid, peers);

		for_each_set_bit(nid, page->owners, MAX_POPCORN_NODES) {
			pid_t pid = rc->remote_tgids[nid];
			if (nid == except_nid) continue;

			__revoke_page_ownership(tsk, nid, pid, addr, ws->id);
			clear_bit(nid, page->owners);
		}

		wait_at_station(ws);
		put_wait_station(tsk->pid, ws);

		put_task_remote(tsk);
	}
}

static remote_page_response_t *__get_remote_page(struct task_struct *tsk, unsigned long addr, unsigned long fault_flags)
{
	remote_page_response_t *rp;
	struct wait_station *ws = get_wait_station(tsk->pid, 1);

	printk("  [%d]: Fetch %lx from origin %d\n",
			tsk->pid, addr, tsk->origin_nid);

	__fetch_remote_page(tsk, tsk->origin_nid, tsk->origin_pid,
			addr, fault_flags, ws->id);

	wait_at_station(ws);
	rp = ws->private;
	put_wait_station(tsk->pid, ws);

	return rp;
}


static remote_page_response_t *__claim_remote_page(struct task_struct *tsk, unsigned long addr, unsigned long fault_flags, struct page *page)
{
	int peers = bitmap_weight(page->owners, MAX_POPCORN_NODES);
	unsigned int random = prandom_u32();
	struct wait_station *ws;
	struct remote_context *rc = get_task_remote(tsk);
	remote_page_response_t *rp;
	int from, nid;

	if (test_bit(my_nid, page->owners)) {
		peers--;
	}
	from = random % peers;

	printk("  [%d]: Fetch %lx from %d peers\n",
			tsk->pid, addr, peers);

	if (fault_for_read(fault_flags)) {
		peers = 0;
	} else {
		peers--;
	}

	// count = one for fetch, peers for revocation
	ws = get_wait_station(tsk->pid, peers + 1);

	for_each_set_bit(nid, page->owners, MAX_POPCORN_NODES) {
		pid_t pid = rc->remote_tgids[nid];
		if (nid == my_nid) continue;
		if (from-- == 0) {
			__fetch_remote_page(tsk, nid, pid, addr, fault_flags, ws->id);

			if (fault_for_read(fault_flags)) break;
			continue;
		}
		if (fault_for_write(fault_flags)) {
			__revoke_page_ownership(tsk, nid, pid, addr, ws->id);
			clear_bit(nid, page->owners);
		}
	}

	wait_at_station(ws);
	rp = ws->private;
	put_wait_station(tsk->pid, ws);

	put_task_remote(tsk);
	return rp;
}


static int __handle_remotefault_at_remote(struct task_struct *tsk, struct mm_struct *mm, struct vm_area_struct *vma, remote_page_request_t *req, remote_page_response_t *res)
{
	unsigned long addr = req->addr;
	unsigned char *paddr;
	struct page *page;
	unsigned fault_flags = req->fault_flags | FAULT_FLAG_REMOTE;

	spinlock_t *ptl;
	pmd_t *pmd;
	pte_t *pte;
	pte_t entry;

	DEFINE_WAIT(wait);
	struct fault_handle *fh;
	bool leader;

	pte = __get_pte_at(mm, vma, addr, &pmd, &ptl);
	if (!pte) {
		return VM_FAULT_OOM;
	}
	if (pte_none(*pte)) {
		pte_unmap(pte);
		return VM_FAULT_SIGSEGV;
	}

	do {
		spin_lock(ptl);
	} while (!__start_fault_handling(tsk, addr,
				fault_flags, ptl, &wait, &fh, &leader));

	page = vm_normal_page(vma, addr, *pte);
	BUG_ON(!page);
	get_page(page);

	BUG_ON(!page_is_mine(page));

	spin_lock(ptl);
	if (fault_for_write(fault_flags)) {
		// drop my ownership
		entry = pte_make_invalid(*pte);
		clear_bit(my_nid, page->owners);
	} else {
		entry = pte_wrprotect(*pte);
	}

	set_pte_at_notify(mm, addr, pte, entry);
	update_mmu_cache(vma, addr, pte);
	flush_tlb_page(vma, addr);

	lock_page(page);
	paddr = kmap_atomic(page);
	memcpy(res->page, paddr, PAGE_SIZE);
	kunmap_atomic(paddr);
	unlock_page(page);
	put_page(page);

	pte_unmap_unlock(pte, ptl);

	__continue_fault_handling(fh);

	return 0;
}


int handle_pte_fault_origin(struct mm_struct *mm,
		struct vm_area_struct *vma, unsigned long address,
		pte_t *pte, pmd_t *pmd, unsigned int flags);

static int __handle_remotefault_at_origin(struct task_struct *tsk, struct mm_struct *mm, struct vm_area_struct *vma, remote_page_request_t *req, remote_page_response_t *res)
{
	int from = req->origin_nid;
	unsigned long addr = req->addr;
	unsigned long fault_flags = req->fault_flags | FAULT_FLAG_REMOTE;
	unsigned char *paddr;
	struct page *page;
	int ret;

	spinlock_t *ptl;
	pmd_t *pmd;
	pte_t *pte;

	DEFINE_WAIT(wait);
	struct fault_handle *fh;
	bool leader;

again:
	pte = __get_pte_at(mm, vma, addr, &pmd, &ptl);
	if (!pte) {
		printk("  [%d]: no pte\n", tsk->pid);
		return VM_FAULT_OOM;
	}

	if (pte_none(*pte)) {
		printk("  [%d]: handle local fault at origin\n", tsk->pid);

		ret = handle_pte_fault_origin(mm, vma, addr, pte, pmd, fault_flags);
		if (ret) {
			return ret;
		}
		goto again;
	}

	do {
		spin_lock(ptl);

		page = vm_normal_page(vma, addr, *pte);
		BUG_ON(!page);
	} while (!__start_fault_handling(tsk, addr,
				fault_flags, ptl, &wait, &fh, &leader));

	get_page(page);

	if (leader) {
		pte_t entry = *pte;

		/* Prepare the page if it is not mine. This should be leader */
		printk("  [%d]: %lx %s\n", tsk->pid, pte_flags(entry),
				page_is_mine(page) ? "mine" : "others");

		if (!page_is_mine(page)) {
			void *paddr;
			remote_page_response_t *rp =
				__claim_remote_page(tsk, addr, fault_flags, page);

			lock_page(page);
			paddr = kmap_atomic(page);
			memcpy(paddr, rp->page, PAGE_SIZE);
			kunmap_atomic(paddr);
			unlock_page(page);

			pcn_kmsg_free_msg(rp);
		}

		if (fault_for_write(fault_flags)) {

			entry = pte_make_invalid(entry);
			clear_bit(my_nid, page->owners);
		} else {
			entry = pte_wrprotect(entry);
			set_bit(my_nid, page->owners);
		}

		spin_lock(ptl);

		set_pte_at_notify(mm, addr, pte, entry);
		update_mmu_cache(vma, addr, pte);
		flush_tlb_page(vma, addr);

		pte_unmap_unlock(pte, ptl);
	}

	set_bit(from, page->owners);

	lock_page(page);
	paddr = kmap_atomic(page);
	memcpy(res->page, paddr, PAGE_SIZE);
	kunmap_atomic(paddr);
	unlock_page(page);

	__continue_fault_handling(fh);

	put_page(page);
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
	int from = req->origin_nid;

	while (!res) {	/* response contains a page. allocate from a heap */
		res = kmalloc(sizeof(*res), GFP_KERNEL);
	}

	tsk = __get_task_struct(req->remote_pid);
	if (!tsk) {
		res->result = VM_FAULT_SIGBUS;
		printk("  [%d]: not found\n", req->remote_pid);
		goto out;
	}
	mm = get_task_mm(tsk);

	printk("\nREMOTE_PAGE_REQUEST [%d]: %lx %s from [%d/%d]\n",
			req->remote_pid, req->addr,
			fault_for_write(req->fault_flags) ? "W" : "R",
			req->origin_pid, from);

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

	res->addr = req->addr;
	res->remote_nid = my_nid;
	res->remote_pid = req->remote_pid;

	req->origin_nid = req->origin_nid;
	req->origin_pid = req->origin_pid;
	res->origin_ws = req->origin_ws;

	pcn_kmsg_send_long(from, res, sizeof(*res));
	printk("->[%d]: %lx %d\n", req->remote_pid, req->addr, res->result);

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
	spinlock_t *ptl;
	struct page *page;
	bool populated = false;
	struct mem_cgroup *memcg;

	DEFINE_WAIT(wait);
	struct fault_handle *fault;
	bool leader;
	remote_page_response_t *rp;

	if (anon_vma_prepare(vma)) {
		BUG_ON("Cannot prepare vma for anonymous page");
		pte_unmap(pte);
		return VM_FAULT_SIGBUS;
	}

	ptl = pte_lockptr(mm, pmd);
	do {
		spin_lock(ptl);

		if (!pte_same(*pte, pte_val)) {
			pte_unmap_unlock(pte, ptl);
			printk("  [%d]: %lx already handled\n", current->pid, addr);
			return 0;
		}
	} while (!__start_fault_handling(current, addr,
				fault_flags, ptl, &wait, &fault, &leader));

	if (!leader) {
		goto out;
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

	rp = __get_remote_page(current, addr, fault_flags);

	spin_lock(ptl);
	if (populated) {
		void *paddr;
		paddr = kmap_atomic(page);
		memcpy(paddr, rp->page, PAGE_SIZE);
		kunmap_atomic(paddr);

		do_set_pte(vma, addr, page, pte, fault_for_write(fault_flags), true);
		mem_cgroup_commit_charge(page, memcg, false);
		lru_cache_add_active_or_unevictable(page, vma);
	} else {
		__map_remote_page(mm, vma, addr, fault_flags, pte, page, rp);
	}
	pte_unmap_unlock(pte, ptl);

	put_page(page);
	pcn_kmsg_free_msg(rp);

out:
	__continue_fault_handling(fault);
	return 0;
}


static int __handle_localfault_at_origin(struct mm_struct *mm,
		struct vm_area_struct *vma, unsigned long addr,
		pmd_t *pmd, pte_t *pte, pte_t pte_val, unsigned int fault_flags)
{
	struct page *page;
	spinlock_t *ptl;

	struct fault_handle *fh;
	DEFINE_WAIT(wait);
	bool leader;

	/* Fresh access to the address. Handle locally since we are at the origin */
	if (pte_none(pte_val)) {
		BUG_ON(pte_present(pte_val));

		printk("  [%d]: fresh at origin. continue\n", current->pid);
		return VM_FAULT_CONTINUE;
	}

	ptl = pte_lockptr(mm, pmd);

	do {
		spin_lock(ptl);

		if (!pte_same(*pte, pte_val)) {
			pte_unmap_unlock(pte, ptl);
			printk("  [%d]: %lx already handled\n", current->pid, addr);
			return 0;
		}

		page = vm_normal_page(vma, addr, pte_val);
		if (page == NULL || !page_is_replicated(page)) {
			spin_unlock(ptl);

			/* Nothing to do with DSM (e.g. COW). Handle locally */
			printk("  [%d]: local at origin. continue\n", current->pid);
			return VM_FAULT_CONTINUE;
		}
	} while (!__start_fault_handling(current, addr,
				fault_flags, ptl, &wait, &fh, &leader));

	/* Handle replicated page via the dsm protocol */
	get_page(page);

	printk("  [%d]: replicated page at origin, %s, %s\n",
			current->pid, page_is_mine(page) ? "mine" : "others",
			leader ? "leader" : "follower");

	if (!leader) {
		goto out_wakeup;
	}

	if (page_is_mine(page)) {
		BUG_ON(fault_for_read(fault_flags));

		__claim_local_page(current, addr, page, my_nid);

		spin_lock(ptl);
		BUG_ON(!pte_same(*pte, pte_val));

		pte_val = pte_make_valid(pte_val);
		pte_val = pte_mkwrite(pte_val);
		pte_val = pte_mkdirty(pte_val);

		set_pte_at_notify(mm, addr, pte, pte_val);
		update_mmu_cache(vma, addr, pte);
		flush_tlb_page(vma, addr);
	} else {
		remote_page_response_t *rp =
				__claim_remote_page(current, addr, fault_flags, page);

		spin_lock(ptl);
		__map_remote_page(mm, vma, addr, fault_flags, pte, page, rp);
		pcn_kmsg_free_msg(rp);
	}
	pte_unmap_unlock(pte, ptl);

out_wakeup:
	__continue_fault_handling(fh);

	put_page(page);
	return 0;
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
			fault_flags, pte_flags(pte_val));

	/*
	 * Thread at the origin
	 */
	if (!current->at_remote) {
		return __handle_localfault_at_origin(
				mm, vma, addr, pmd, pte, pte_val, fault_flags);
	}

	/*
	 * Thread running at a remote
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

	if (!pte_present(pte_val)) {
		/* Remote page fault */
		return __handle_localfault_at_remote(
				mm, vma, addr, pmd, pte, pte_val, fault_flags);
	}

	if (fault_flags & FAULT_FLAG_TRIED) {
		/* Some do_fault() makes the fault to be called again. */
		return VM_FAULT_CONTINUE;
	}

	if ((vma->vm_flags & VM_WRITE) &&
			fault_for_write(fault_flags) && !pte_write(pte_val)) {
		/* wr-protected for keeping page consistency */
		return __handle_localfault_at_remote(
				mm, vma, addr, pmd, pte, pte_val, fault_flags);
	}

	printk("  [%d]: might be fixed by others???\n", current->pid);
	return 0;
}


/**************************************************************************
 * Routing popcorn messages to worker
 */

DEFINE_KMSG_WQ_HANDLER(remote_page_request);
DEFINE_KMSG_NONBLOCK_WQ_HANDLER(remote_page_response);
DEFINE_KMSG_NONBLOCK_WQ_HANDLER(page_invalidate_request);
DEFINE_KMSG_NONBLOCK_WQ_HANDLER(page_invalidate_response);
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
