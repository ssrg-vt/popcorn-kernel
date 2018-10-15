/*
 * sync.c
 * Copyright (C) 2018 Ho-Ren(Jack) Chuang <horenc@vt.edu>
 *
 * Distributed under terms of the MIT license.
 *
 * TODO: take ip into consideration
 */
#include <linux/syscalls.h>
#include <linux/pagemap.h>

#include <popcorn/types.h>
#include <popcorn/debug.h>
#include <popcorn/bundle.h>

#include "process_server.h"
#include "wait_station.h"
#include "page_server.h"
#include "types.h"

#include "trace_events.h"
#include <linux/delay.h>
#include <linux/mm.h>

#define POPCORN_TSO_BARRIER 1
#define REENTRY_BEGIN_DISABLE 1 // 0: repeat show 1: once

#define BARRIER_INFO 0
#if BARRIER_INFO
#define BARRPRINTK(...) printk(KERN_INFO __VA_ARGS__)
#else
#define BARRPRINTK(...)
#endif

#define BARRIER_INFO_MORE 0
#if BARRIER_INFO_MORE
#define BARRMPRINTK(...) printk(KERN_INFO __VA_ARGS__)
#else
#define BARRMPRINTK(...)
#endif

#define SYNC_DEBUG_THIS 0
#if SYNC_DEBUG_THIS
#define SYNCPRINTK2(...) printk(KERN_INFO __VA_ARGS__)
#else
#define SYNCPRINTK2(...)
#endif

#define DIFF_APPLY 0
#if DIFF_APPLY
#define DIFFPRINTK(...) printk(KERN_INFO __VA_ARGS__)
#else
#define DIFFPRINTK(...)
#endif

#define MSG_DBG 0 /* TODO */
#if MSG_DBG
#define _MSGPRINTK(...) printk(KERN_INFO __VA_ARGS__)
#else
#define _MSGPRINTK(...)
#endif

#define BUF_DBG 0
#if BUF_DBG
#define SYNCPRINTK3(...) printk(KERN_INFO __VA_ARGS__)
#else
#define SYNCPRINTK3(...)
#endif

#define RGN_DEBUG_THIS 0
#if RGN_DEBUG_THIS
#define RGNPRINTK(...) printk(KERN_INFO __VA_ARGS__)
#else
#define RGNPRINTK(...)
#endif

static int violation = 0;

unsigned long long system_tso_wr_cnt = 0;
unsigned long long system_tso_nobenefit_region_cnt = 0;
spinlock_t tso_lock;

static bool print = false;

/* ugly */
extern void __print_pids(struct remote_context *rc);
extern pte_t *get_pte_at(struct mm_struct *mm, unsigned long addr, pmd_t **ppmd, spinlock_t **ptlp);
/*************************
 * for barriers/fence
 */
void __clean_perthread_inv_buf(int inv_cnt)
{
	int j;
	for (j = 0; j < inv_cnt; j++)
		current->buffer_inv_addrs[j] = 0;
}

#if GLOBAL
void __clean_global_inv(struct remote_context *rc)
{
	spin_lock(&rc->inv_lock);
	memset(rc->inv_addrs, 0, sizeof(*rc->inv_addrs) * rc->inv_cnt);
	if (!NOCOPY_NODE)
		memset(rc->inv_pages, 0, rc->inv_cnt * PAGE_SIZE);
	rc->inv_cnt = 0;
	spin_unlock(&rc->inv_lock);
}
#endif


//PCN_KMSG_TYPE_PAGE_MERGE_DONE_REQUEST /* tell remote */ /* handshak */
/* [leader all done/handshak] -> remote */
static void	__sned_handshake(struct remote_context *rc, int nid)
{
	remote_baiier_done_request_t req = {
        .origin_pid = rc->remote_tgids[nid],
    };

	/* tell remote */ /* handshak */
    pcn_kmsg_send(PCN_KMSG_TYPE_PAGE_MERGE_DONE_REQUEST,
								nid, &req, sizeof(req));
}


/* (serial/leader)[Local] -> */
extern int sync_server_local_conflictions(struct remote_context *rc);
extern int sync_server_local_serial_conflictions(struct remote_context *rc);
void __locally_find_conflictions(int nid, struct remote_context *rc)
{
    //page_merge_request_t *req = pcn_kmsg_get(sizeof(*req));
    page_merge_request_t *req = kmalloc(sizeof(*req), GFP_KERNEL);
	struct wait_station *ws = get_wait_station(current);
	int iter = 0;
	int sent_cnt = 0;
	int single_sent = 0;
	int total_iter;
	bool zero_case = true;
	int local_wr_cnt;
	BUG_ON(!req);
	BUG_ON(rc->inv_cnt > MAX_ALIVE_THREADS * MAX_WRITE_INV_BUFFERS);

	req->origin_pid = current->pid;
    req->origin_ws = ws->id;
    req->remote_pid = rc->remote_tgids[nid];
#if !GLOBAL
	/* implementation - local */
	local_wr_cnt = sync_server_local_conflictions(rc);
	/* dbg */
	if (rc->lconf_cnt)
		BARRMPRINTK("local_conflict_addr_cnt %d\n", rc->lconf_cnt);
	BUG_ON(rc->lconf_cnt > MAX_ALIVE_THREADS * MAX_WRITE_INV_BUFFERS);

#else
	/* implementation - global */
	local_wr_cnt = sync_server_local_serial_conflictions(rc);
	rc->sys_rw_cnt += local_wr_cnt;
#endif
	/* RC: make sure list is ready */
	rc->ready = true;
	rc->diffs++;
	rc->local_merge_id++;
	smp_wmb(); /* different cpu */

	req->merge_id = rc->local_merge_id;
	SYNCPRINTK2("mg: l lr(%d/%d)\n", rc->local_merge_id, rc->remote_merge_id);

	/* general */
	/* scatters - send - even send when 0 */
	if (local_wr_cnt == 0) { /* special zero case 0/0 */
		iter = 0; /* total == 0 iter == 0 */
		total_iter = 0;
		atomic_set(&rc->scatter_pendings, 1);
		// current->tso_nobenefit_region_cnt++; /* TODO: use rc and function */
	} else {
		iter++; /* total > 0 iter start from 1 */
		total_iter = ((local_wr_cnt + MAX_WRITE_INV_BUFFERS - 1) /
											MAX_WRITE_INV_BUFFERS);
		zero_case = false;
		atomic_set(&rc->scatter_pendings, total_iter);
	}

	do {
		single_sent = local_wr_cnt - sent_cnt;
		if (single_sent > (int)MAX_WRITE_INV_BUFFERS)
			single_sent = MAX_WRITE_INV_BUFFERS;

		/* put more handshake info to detect skew cases */
		//req->begin =
		//req->fence =
		//req->end =
		req->origin_ws = ws->id;

		req->wr_cnt = single_sent;
		req->iter = iter;
		req->total_iter = total_iter;

		/* optmization: remove this copy by moving into the func() */
		/* read from "rc->inv_addrs" now. optimize - remove rc->inv_addrs */
		if (single_sent) {
			memcpy(req->addrs, rc->inv_addrs + sent_cnt,
								sizeof(*req->addrs) * single_sent);
		}

		/* optimize - send_size = sizeof(*req) -
		 *							sizeof(req->addrs) +
		 *							(sizeof(*req->addrs) * single_sent)
		 */
		/* TODO: potential BUG. better to use pending */
		BARRMPRINTK("[%d]/%d: this %d / sent %d / local_wr_cnt %d MERGE req ->\n",
						iter, total_iter, single_sent, sent_cnt, local_wr_cnt);

		/* optimization: these sends can be parallel */
		/* BUG: using pcn_kmsg_post will have problem */
		/* TODO: be careful of using pcn_kmsg_send() */
		/* For the > 2k case:
		 * pcn_kmsg_get() + pcn_kmsg_post = immediately free at completion
		 * pcn_kmsg_get() / kmalloc + pcn_kmsg_send => copy
		 * For the < 2k case:
		 * pcn_kmsg_get() + pcn_kmsg_post = immediately free at completion
		 * pcn_kmsg_get() / kmalloc  + pcn_kmsg_send => copy
		 */
		pcn_kmsg_send(PCN_KMSG_TYPE_PAGE_MERGE_REQUEST, nid, req,
								sizeof(*req) - sizeof(req->addrs) +
								(sizeof(*req->addrs) * single_sent));
		SYNCPRINTK2("-> MERGE sent l(%d)\n", rc->local_merge_id);

		iter++;
		sent_cnt += single_sent;

		if (zero_case)	/* redundant? */
			break;
	} while (sent_cnt < local_wr_cnt);

	SYNCPRINTK2("-> MERGE sent l go to sleep(%d)\n", rc->local_merge_id);
	/* wait until the last scatter requestion done + diff_req__from_remote done */
	wait_at_station(ws);
	kfree(req); /* conservative */

	/* TODO: make sure no diffs left diff may still oerating!!!!! */
	/* TODO: make sure no diffs left diff may still oerating!!!!! */
	/* TODO: make sure no diffs left diff may still oerating!!!!! */
	rc->diffs--;
	while(rc->diffs) { io_schedule(); BUG_ON(rc->diffs < 0); } /* wait untile local apply_diffs are done as well */

	//while(rc->diffs && !rc->is_diffed) { smp_rmb(); BUG_ON(rc->diffs < 0); }
//		while(rc->diffs) { smp_rmb(); BUG_ON(rc->diffs < 0); }
	/* wait untile local apply_diffs are done as well */

	/* find a proper place as long as in leader before END_B */
	/* prevent from double handshake (imbalance) */
	SYNCPRINTK2("mg: wait lr(%d/%d)\n", rc->local_merge_id, rc->remote_merge_id);
	//while(rc->local_merge_id != rc->remote_merge_id) { io_schedule(); smp_mb(); }
	/* TODO: BUG() think hard */
	while(rc->local_merge_id > rc->remote_merge_id) { io_schedule(); smp_mb(); }
	SYNCPRINTK2("mg: pass lr(%d/%d)\n", rc->local_merge_id, rc->remote_merge_id);

	rc->local_done_cnt++;
	smp_wmb();
	SYNCPRINTK2("\t-> handshake lr(%d/%d) diffs %d is_diffed %d remote_done %d\n",
			rc->local_done_cnt, rc->remote_done_cnt,
			rc->diffs, rc->is_diffed, rc->remote_done);
	if (my_nid == 0)
		__sned_handshake(rc, 1);
	else
		__sned_handshake(rc, 0);

}

extern int do_locally_inv_page(struct task_struct *tsk, unsigned long addr);
/* optimize: overwrite the vma buffer for performance */
static void __make_diff(char *new, char *origin, page_diff_apply_request_t *req)
{
	int i;
#ifdef CONFIG_POPCORN_CHECK_SANITY
	int good = 0;
#endif
	for (i = 0; i < PAGE_SIZE; i++)
		*(req->diff_page + i) = *(new + i) ^ *(origin + i);

#ifdef CONFIG_POPCORN_CHECK_SANITY
	for (i = 0; i < PAGE_SIZE; i++) {
		if(*(origin + i)) {
			good = 1; break;
		}
	}
//	BUG_ON(!good && "origin page is all zero") /* TODO: BUG remote first? zpg !!!!*/;
	good = 0;
	for (i = 0; i < PAGE_SIZE; i++) {
		if(*(new + i) ^ *(origin + i)) {
			good = 1; break;
		}
	}
	// BUG_ON(!good && "no diff"); /* possible? write 1 + write 0 => 0*/
#endif
}

/* -> [Remote][diff_req] -> back(local) */
/* todo: merge with __do_apply_diff() */
static void __make_diffs_send_back_at_remote(struct task_struct *tsk, struct remote_context *rc, int local_ofs, int nid)
{
	page_diff_apply_request_t *req = kmalloc(sizeof(*req), GFP_KERNEL);
	//page_diff_apply_request_t *req = pcn_kmsg_get(sizeof(*req));
	unsigned long conflict_addr = rc->inv_addrs[local_ofs]; /* local addr */
	struct vm_area_struct *vma;
    spinlock_t *ptl;
    pmd_t *pmd;
    pte_t *pte;
	struct page *page;
	void *paddr;
#ifdef CONFIG_POPCORN_CHECK_SANITY
	int i, good = 0;
#endif
	BUG_ON(!req);
	BUG_ON(!conflict_addr);
#ifdef CONFIG_POPCORN_CHECK_SANITY
	for (i = 0; i < PAGE_SIZE; i++)
		*(req->diff_page + i) = 0;
#endif

	down_read(&tsk->mm->mmap_sem);

	vma = find_vma(tsk->mm, conflict_addr);
	BUG_ON(!vma);
	if (vma->vm_start > conflict_addr) {
		PCNPRINTK_ERR("**** vma->vm_start %lx > conflict_addr %lx ******\n",
						vma->vm_start, conflict_addr);
		BUG_ON(vma->vm_start > conflict_addr);
	}
    pte = get_pte_at(tsk->mm, conflict_addr, &pmd, &ptl);
	BUG_ON(!pte);

	page = vm_normal_page(vma, conflict_addr, *pte);
	BUG_ON(!page);

	get_page(page);
	paddr = kmap(page);
	BUG_ON(!paddr);
	__make_diff(paddr,  rc->inv_pages + (local_ofs * PAGE_SIZE), req);
	kunmap(page);
	put_page(page);

	pte_unmap(pte);

	up_read(&tsk->mm->mmap_sem);

#ifdef CONFIG_POPCORN_CHECK_SANITY
	/* immediately remote after working */
	for (i = 0; i < PAGE_SIZE; i++) {
		if (*(req->diff_page + i) != 0) { /* any of them is diff */
			good = 1;
			break;
		}
	}
	// BUG_ON(!good && "no diff"); /* possible? write 1 + write 0 => 0*/
#endif

    req->origin_pid = rc->remote_tgids[nid],
	req->remote_pid = current->pid,
	req->diff_addr = conflict_addr,

	/* make sure the send order */
    pcn_kmsg_send(PCN_KMSG_TYPE_PAGE_DIFF_APPLY_REQUEST,
								nid, req, sizeof(*req));
	kfree(req);
}

static void __apply_diff(char *target, char *diff)
{
	int i;
	for (i = 0; i < PAGE_SIZE; i++)
		*(target + i) = *(target + i) ^ *(diff + i);
}

/* todo: merge with __make_diffs_send_back_at_remote() */
void __do_apply_diff(page_diff_apply_request_t *req, struct task_struct *tsk)
{
	unsigned long conflict_addr = req->diff_addr;
	struct vm_area_struct *vma;
    spinlock_t *ptl;
    pmd_t *pmd;
    pte_t *pte;
	struct page *page;
	void *paddr;
	BUG_ON(!conflict_addr);

	down_read(&tsk->mm->mmap_sem);

	vma = find_vma(tsk->mm, conflict_addr);
	BUG_ON(!vma);
	if (vma->vm_start > conflict_addr) {
		PCNPRINTK_ERR("**** vma->vm_start %lx > conflict_addr %lx ******\n",
						vma->vm_start, conflict_addr);
		BUG_ON(vma->vm_start > conflict_addr);
	}
    pte = get_pte_at(tsk->mm, conflict_addr, &pmd, &ptl);
	BUG_ON(!pte);

	page = vm_normal_page(vma, conflict_addr, *pte);
	BUG_ON(!page);

	get_page(page);
	paddr = kmap(page);
	BUG_ON(!paddr);
	BUG_ON(my_nid != NOCOPY_NODE);
	__apply_diff(paddr, req->diff_page);
	kunmap(page);
	put_page(page);

	pte_unmap(pte);

	up_read(&tsk->mm->mmap_sem);
}

/* diff_req -> back[Local] */
static int handle_page_apply_diff_request(struct pcn_kmsg_message *msg)
{
    page_diff_apply_request_t *req = (page_diff_apply_request_t *)msg;
	struct task_struct *tsk = __get_task_struct(req->origin_pid);
	struct remote_context *rc;
	BUG_ON(!tsk);
	rc = get_task_remote(tsk);
	BUG_ON(!rc);
	DIFFPRINTK("[%d] applying diff 0x%lx\n", tsk->pid, req->diff_addr);

	rc->diffs++; /* TODO race condition */
	rc->sys_ww_cnt++;
	smp_wmb();

	__do_apply_diff(req, tsk);

	rc->diffs--; /* TODO race condition*/
//	rc->is_diffed = true;
	smp_wmb();

	__put_task_remote(rc);
    put_task_struct(tsk);
	pcn_kmsg_done(req);
	return 0;
}

/* Kernel worker at remote */
/* -> [Remote] */
int __find_collision_btw_nodes_at_remote(page_merge_request_t *req, page_merge_response_t *res, struct remote_context *rc, struct task_struct *tsk)
{
	int i, j, diff_cnt = 0, from_nid = PCN_KMSG_FROM_NID(req);

	BARRMPRINTK("prepare to find conflict at remote req->wr_cnt %lu (wait rc->ready)\n", req->wr_cnt);
	/* RC: make sure global list is ready - wait on cache line */
	while(!rc->ready) { smp_rmb(); io_schedule(); }
	/* TODO bug spinging on bare now */
	BARRMPRINTK("find conflict at remote req->wr_cnt %lu (barrier)\n", req->wr_cnt);
#ifdef CONFIG_POPCORN_CHECK_SANITY
	if (req->wr_cnt < 5 && rc->inv_cnt < 5) {
		for (i = 0; i < req->wr_cnt; i++)
			SYNCPRINTK2("rlist: 0x%lx\n", req->addrs[i]);

		for (i = 0; i < rc->inv_cnt; i++)
			SYNCPRINTK2("llist: 0x%lx\n", rc->inv_addrs[i]);
	}
#endif
	for (i = 0; i < req->wr_cnt; i++) {
		/* If no collision, inv.
		 * If collision, if !NOCOPY_NODE, generate diffs + inv + send merge reeq */
		unsigned long target_addr = req->addrs[i];
		bool conflict = false;
		int ret;
		BUG_ON(!target_addr);
		for (j = 0; j < rc->inv_cnt; j++) {
			unsigned long addr = rc->inv_addrs[j];
			if (target_addr == addr) {
				conflict = true;
				break;
			}
		}

		if (!conflict) {
			/* optimize: delay */
			SYNCPRINTK3("[ar/%d/%3d] inv 0x%lx\n", my_nid, i, target_addr);
			ret = do_locally_inv_page(tsk, target_addr);
			if (ret)
				BUG();
		} else { /* both will detect the same conflict addrs */
			if (my_nid != NOCOPY_NODE) {
				/* optimize: batch */
				SYNCPRINTK3("[ar/%d/%3d] ww 0x%lx\n", my_nid, i, target_addr);
				ret = do_locally_inv_page(tsk, target_addr);
				if (ret)
					BUG();
				__make_diffs_send_back_at_remote(tsk, rc, j, from_nid);
				diff_cnt++;
			} else {
				if (my_nid == 0) { /* only origin maintain pip */
				}
			}
		}
	}

	/* cross check */
	rc->remote_sys_ww_cnt += diff_cnt;
	return diff_cnt;
}

/* -> [Remote] */
static void process_page_merge_request(struct work_struct *work)
{
	START_KMSG_WORK(page_merge_request_t, req, work);
    //page_merge_response_t *res = pcn_kmsg_get(sizeof(*res));
    page_merge_response_t *res = kmalloc(sizeof(*res), GFP_KERNEL);
	struct task_struct *tsk = __get_task_struct(req->remote_pid);
	struct remote_context *rc;
	BUG_ON(!tsk);
	rc = get_task_remote(tsk);
	BUG_ON(!res|| !rc);
	SYNCPRINTK2("\t\t<- MERGE request\n");

	/* put more handshake info to detect skew cases */
	//req->begin =
	//req->fence =
	//req->end =
	// handshak remote status
	rc->remote_merge_id = req->merge_id;
	smp_mb();
	SYNCPRINTK2("mg: r lr(%d/%d)\n", rc->local_merge_id, rc->remote_merge_id);

	res->scatters = __find_collision_btw_nodes_at_remote(req, res, rc, tsk);

	BARRMPRINTK("\t\t{{{ diff_cnt at remote %d/%lu }}}\n", res->scatters, req->wr_cnt);
//	if (res->scatters)
//		BUG_ON(res->diffs); // hard to detect

	res->origin_pid = req->origin_pid;
    res->origin_ws = req->origin_ws;
    //res->remote_pid = req->remote_pid;

	/* not only 1 page man... 32k-4 8-1 = 7diffs......*/
//	res->scatters = total; /* not used */
//	res->merge_id = iter_num;

	res->wr_cnt = req->wr_cnt;
	res->iter = req->iter;
	res->total_iter = req->total_iter;

	/* causion: more than RR */
	/* optimize: send_size (now > 1 pg) */
    //pcn_kmsg_post(PCN_KMSG_TYPE_PAGE_MERGE_RESPONSE,
    pcn_kmsg_send(PCN_KMSG_TYPE_PAGE_MERGE_RESPONSE,
					PCN_KMSG_FROM_NID(req), res, sizeof(*res));
	kfree(res);

	__put_task_remote(rc);
    put_task_struct(tsk);
    END_KMSG_WORK(req);
}


/* handshak -> [remote] */
static int handle_page_diff_all_done_request(struct pcn_kmsg_message *msg)
{
    remote_baiier_done_request_t *req = (remote_baiier_done_request_t *)msg;
	/* TODO: change name */
	struct task_struct *tsk = __get_task_struct(req->origin_pid);
	struct remote_context *rc = get_task_remote(tsk);

	rc->remote_done = true;
	rc->remote_done_cnt++;
	smp_wmb();
	SYNCPRINTK2("\t<- handshake lr(%d/%d) diffs %d is_diffed %d remote_done %d\n",
			rc->local_done_cnt, rc->remote_done_cnt,
			rc->diffs, rc->is_diffed, rc->remote_done);

	__put_task_remote(rc);
    put_task_struct(tsk);
	pcn_kmsg_done(req);
	return 0;
}



// * 1. fixup (async single now)
/* -> back[Local](main)(many_scatters) */
static int handle_page_merge_response(struct pcn_kmsg_message *msg)
{
    page_merge_response_t *res = (page_merge_response_t *)msg;
    struct wait_station *ws = wait_station(res->origin_ws);
	struct task_struct *tsk = __get_task_struct(res->origin_pid);
	struct remote_context *rc = get_task_remote(tsk);
	BUG_ON(!rc || !tsk);
//	if (!res->scatters) /* no need to merge */
//		goto done;

	/* merges are all done in diff_apply per page */

	//res->wr_cnt = req->wr_cnt;
	//res->iter = req->iter;
	//res->total_iter = req->total_iter;

//done:
	/* last MERGE response */
	if (atomic_dec_return(&rc->scatter_pendings) == 0) { /* done */
		SYNCPRINTK2("\t\t<- MERGE response [*] wakeup [*]\n");
		complete(&ws->pendings); /* wake up leader thread */
	} else {
		SYNCPRINTK2("\t\t<- MERGE response [ ]\n");
	}
	BUG_ON(atomic_read(&rc->scatter_pendings) < 0);

	__put_task_remote(rc);
    put_task_struct(tsk);
    pcn_kmsg_done(res);
    return 0;
}

/* clean per barrier data in rc */
static void __per_barrier_reset(struct remote_context *rc)
{
	rc->ready = false;
//	rc->is_diffed = false;
	rc->remote_done = false;
	if (rc->diffs) {
		PCNPRINTK_ERR("rc->diffs %d\n", rc->diffs);
		BUG_ON(rc->diffs); // auto maintained
	}
#if GLOBAL
	/* clean all for each barrier/fence */
	__clean_global_inv(rc);
#endif
	smp_wmb();
}

/*
 * put rc and pirnk outsite !!!!!!!!!!1
 * be mor general
 */
static bool __popcorn_barrier_begin(struct remote_context *rc, int a)
{
	bool leader = false;
	int left_t = atomic_dec_return(&rc->barrier);
	BUG_ON(rc->threads_cnt > 96);

	if (left_t == 0) {
		leader = true;
		BARRPRINTK("=== +++[*%d] BARRIER %5s! line %d mb %lu b %lu f %lu t %d "
				"Hand Shake done. ===\n",
				current->pid, "BEGIN", a, current->begin_m_cnt,
				current->begin_cnt, current->tso_fence_cnt, rc->threads_cnt);
	}

	return leader;
}

static void __popcorn_barrier_end(struct remote_context *rc, int a, bool leader)
{
	if (leader) {
		BARRPRINTK("=== ---[*%d] BARRIER %5s! line %d mb %lu b %lu f %lu t %d "
					"Hand Shake done. ===\n\n",
					current->pid, "END", a, current->begin_m_cnt,
					current->begin_cnt, current->tso_fence_cnt, rc->threads_cnt);

		/* Race Consition !!!!! wait all are in the wq */
		while (atomic_read(&rc->pendings) != rc->threads_cnt - 1)
			io_schedule(); // seems important to yield the atomic op bus in VM

		/* remote also done (handshake) */
		//while (!rc->remote_done)
		//SYNCPRINTK2("\t\t\thandshake local cnt %d\n", rc->local_done_cnt);
		SYNCPRINTK2("\t- wait lr(%d/%d) diffs %d is_diffed %d remote_done %d\n",
					rc->local_done_cnt, rc->remote_done_cnt,
					rc->diffs, rc->is_diffed, rc->remote_done);
		while (rc->local_done_cnt > rc->remote_done_cnt) {
			io_schedule(); smp_rmb();
		}

		SYNCPRINTK2("\t- pass lr(%d/%d)\n",
					rc->local_done_cnt, rc->remote_done_cnt);

		__per_barrier_reset(rc); /* before waking up followers */
		atomic_inc(&rc->pendings);
		atomic_set(&rc->barrier, rc->threads_cnt);
		wake_up_all(&rc->waits);
		atomic_dec(&rc->pendings);

		BARRPRINTK("=== ---[*%d] BARRIER %5s! line %d mb %lu b %lu f %lu t %d "
				"Hand Shake done. ===\n\n",
				current->pid, "END*", a, current->begin_m_cnt,
				current->begin_cnt, current->tso_fence_cnt, rc->threads_cnt);
	} else {
		DEFINE_WAIT(wait);
		//SYNCPRINTK("[ ][%d] left_t %d\n", current->pid, left_t);
        atomic_inc(&rc->pendings);
		prepare_to_wait_exclusive(&rc->waits, &wait, TASK_UNINTERRUPTIBLE);
		io_schedule();
		finish_wait(&rc->waits, &wait);
		atomic_dec(&rc->pendings);
	}
}

void collect_tso_wr(struct task_struct *tsk)
{
	struct remote_context *rc = get_task_remote(tsk);
#if 0
	if (current->accu_tso_wr_cnt) {
		spin_lock(&tso_lock);
		system_tso_wr_cnt += current->accu_tso_wr_cnt;
		system_tso_nobenefit_region_cnt += current->tso_nobenefit_region_cnt;
		spin_unlock(&tso_lock);
		printk("[%d]: exit contributs regions %llu accu_tso %llu "
				"-> system_tso_pg %llu && "
				"empty_region %llu -> system_empty_region %llu "
				"violation %d\n",
				current->pid, current->tso_region_cnt,
				current->accu_tso_wr_cnt,
				system_tso_wr_cnt,
				current->tso_nobenefit_region_cnt,
				system_tso_nobenefit_region_cnt,
				violation);
	}
#endif
	printk("[%d]: exit sys_rw_cnt %lu "
						"sys_ww_cnt %lu (remote side %lu)  "
						"sys_inv_cnt %lu "
						"violation %d\n",
						tsk->pid,
						rc->sys_rw_cnt,
						rc->sys_ww_cnt,
						rc->remote_sys_ww_cnt,
		rc->sys_rw_cnt - rc->sys_ww_cnt - rc->remote_sys_ww_cnt,
						violation);
	__put_task_remote(rc);
}

void clean_tso_wr(void)
{
	spin_lock(&tso_lock);
	system_tso_wr_cnt = 0;
	system_tso_nobenefit_region_cnt = 0;
	spin_unlock(&tso_lock);
}

static int __popcorn_tso_fence(int a, void __user * b)
{
	bool leader = false;
	struct remote_context *rc = get_task_remote(current);
	RGNPRINTK("\t(maybe implicit) [%d] %s():\n", current->pid, __func__);

	if (!current->tso_region) { /* open to detect errors */
		//PCNPRINTK_ERR("[%d] BUG tso_region order violation when "
		//								"\"unlock\"\n", current->pid);
		goto out;
	}

#if POPCORN_TSO_BARRIER
	leader = __popcorn_barrier_begin(rc, a);

	if (leader) {
		if (my_nid == 0)
			__locally_find_conflictions(1, rc);
		else
			__locally_find_conflictions(0, rc);
	}

	__popcorn_barrier_end(rc, a, leader);
#endif

#if 0
	inv_cnt = current->tso_wr_cnt;
	if (inv_cnt) {
		int nid = -1;
		unsigned long *addrs;
		if (my_nid == 0) nid = 1; else nid = 0;

		addrs = current->buffer_inv_addrs;
		SYNCPRINTK("[%d] revoking->@%d addrs[0]=%lx tso_wr_cnt %llu\n",
						current->pid, nid, addrs[0], current->tso_wr_cnt);

		// lock? befor calling this function? check claim_
		remote_revoke_page_ownerships(current, nid, /* 2-node assumption */
								rc->remote_tgids[nid], addrs, inv_cnt);

		SYNCPRINTK("[%d] revoking done  addrs[0]=%lx tso_wr_cnt %llu\n",
							current->pid, addrs[0], current->tso_wr_cnt);

		/* important: makke sure the location */
		__clean_perthread_inv_buf(inv_cnt);
	}

	/* perf statis */
	if (inv_cnt) { //|| !current->tso_wx_cnt)
		//PCNPRINTK_ERR("[%d] WARNNING no benefits here\n", current->pid);
		current->tso_nobenefit_region_cnt++;
	}
#endif

	current->tso_wr_cnt = 0;
	current->tso_wx_cnt = 0;
	current->tso_fence_cnt++;
out:
	put_task_remote(current);
	return 0;
}

/*
 * Syscalls
 */
#ifdef CONFIG_POPCORN
SYSCALL_DEFINE2(popcorn_tso_begin, int, a, void __user *, b)
{
	// TODO: merge to one
	if (current->tso_region || current->tso_wr_cnt || current->tso_wx_cnt) {
		WARN_ON_ONCE("BUG tso_region order violation when \"lock\"");
		if (!print) {
#if REENTRY_BEGIN_DISABLE
			print = true;
#endif
			PCNPRINTK_ERR("[%d] BUG tso_region order violation when \"lock\" "
						"region (%s) tso_wr %llu tso_wx %llu line %d\n",
									current->pid, current->tso_region?"O":"X",
									current->tso_wr_cnt, current->tso_wx_cnt, a);
		}
		violation++;
		//__popcorn_tso_fence(a, b); /* weired case..... but we have to fix NMW */
	}
	current->tso_region = true;
	RGNPRINTK("[%d] %s(): %lu\n", current->pid,
				__func__, current->begin_cnt++);

    //current->tso_region_id = a; // don't uncomment for now
    trace_tso(my_nid, current->pid, a, 'b');

	return 0;
}

SYSCALL_DEFINE2(popcorn_tso_fence, int, a, void __user *, b)
{
	RGNPRINTK("[%d] %s(): id %d\n", current->pid, __func__, a);
    trace_tso(my_nid, current->pid, a, 'f');
	return __popcorn_tso_fence(a, b);
}

SYSCALL_DEFINE2(popcorn_tso_end, int, a, void __user *, b)
{
	RGNPRINTK("[%d] %s(): id %d\n", current->pid, __func__, a);
    trace_tso(my_nid, current->pid, a, 'e');
	__popcorn_tso_fence(a, b);
	current->tso_region = false;
	current->tso_region_id = 0;
	current->tso_region_cnt++;
	return 0;
}

SYSCALL_DEFINE2(popcorn_tso_id, int, a, void __user *, b)
{
	if (current->tso_region) {
		// warnning?
	}
	current->tso_region_id = a;
	RGNPRINTK("[%d] %s(): id %lu\n", current->pid,
				__func__, current->tso_region_id);
	return 0;
}

SYSCALL_DEFINE2(popcorn_tso_begin_manual, int, a, void __user *, b)
{
	// TODO: merge to one
	if (current->tso_region || current->tso_wr_cnt || current->tso_wx_cnt) {
		WARN_ON_ONCE("BUG tso_region order violation when \"lock\"");
		if (!print) {
#if REENTRY_BEGIN_DISABLE
			print = true;
#endif
			PCNPRINTK_ERR("[%d] BUG tso_region order violation when \"lock\" "
							"region (%s) tso_wr %llu tso_wx %llu line %d\n",
							current->pid, current->tso_region?"O":"X",
							current->tso_wr_cnt, current->tso_wx_cnt, a);
		}
		violation++;
		//__popcorn_tso_fence(a, b); /* weired case..... but we have to fix NMW */
	}
	current->tso_region = true;
	RGNPRINTK("[%d] %s(): %lu\n", current->pid,
				__func__, current->begin_m_cnt++);
	return 0;
}

SYSCALL_DEFINE2(popcorn_tso_fence_manual, int, a, void __user *, b)
{
	RGNPRINTK("[%d] %s():\n", current->pid, __func__);
	return __popcorn_tso_fence(a, b);
}

SYSCALL_DEFINE2(popcorn_tso_end_manual, int, a, void __user *, b)
{
	// TODO: check remaining like _begin?
	RGNPRINTK("[%d] %s():\n", current->pid, __func__);
	__popcorn_tso_fence(a, b);
	current->tso_region = false;
	current->tso_region_cnt++;
	return 0;
}
#else // CONFIG_POPCORN
SYSCALL_DEFINE2(popcorn_tso_begin, int, a void __user *, b)
{
	PCNPRINTK_ERR("Kernel is not configured to use popcorn\n");
	return -EPERM;
}

SYSCALL_DEFINE2(popcorn_tso_fence, int, a, void __user *, b)
{
	PCNPRINTK_ERR("Kernel is not configured to use popcorn\n");
	return -EPERM;
}

SYSCALL_DEFINE2(popcorn_tso_end, int, a, void __user *, b)
{
	PCNPRINTK_ERR("Kernel is not configured to use popcorn\n");
	return -EPERM;
}

SYSCALL_DEFINE2(popcorn_tso_id, int, a, void __user *, b)
{
	PCNPRINTK_ERR("Kernel is not configured to use popcorn\n");
	return -EPERM;
}


SYSCALL_DEFINE2(popcorn_tso_begin_manual, int, a void __user *, b)
{
	PCNPRINTK_ERR("Kernel is not configured to use popcorn\n");
	return -EPERM;
}

SYSCALL_DEFINE2(popcorn_tso_fence_manual, int, a, void __user *, b)
{
	PCNPRINTK_ERR("Kernel is not configured to use popcorn\n");
	return -EPERM;
}

SYSCALL_DEFINE2(popcorn_tso_end_manual, int, a, void __user *, b)
{
	PCNPRINTK_ERR("Kernel is not configured to use popcorn\n");
	return -EPERM;
}
#endif

DEFINE_KMSG_WQ_HANDLER(page_merge_request);
int __init popcorn_sync_init(void)
{
	spin_lock_init(&tso_lock);

	REGISTER_KMSG_WQ_HANDLER(
            PCN_KMSG_TYPE_PAGE_MERGE_REQUEST, page_merge_request);
    REGISTER_KMSG_HANDLER(	/* main scatters */
            PCN_KMSG_TYPE_PAGE_MERGE_RESPONSE, page_merge_response);
    REGISTER_KMSG_HANDLER(	/* diff - one way */
            PCN_KMSG_TYPE_PAGE_DIFF_APPLY_REQUEST, page_apply_diff_request);

    REGISTER_KMSG_HANDLER(	/* my side all done signal - one way - only one */
            PCN_KMSG_TYPE_PAGE_MERGE_DONE_REQUEST, page_diff_all_done_request);


	/* dbg */
	printk("si: 8 * %d * %d = [%lu]/ PAGE = [%lu] pgs - "
			"data size [%lu] / PAGE = [%lu] pgs\n",
			(int)MAX_ALIVE_THREADS, (int)MAX_WRITE_INV_BUFFERS,
			(long unsigned int)8 * MAX_ALIVE_THREADS * MAX_WRITE_INV_BUFFERS,
			(long unsigned int)8 * MAX_ALIVE_THREADS * MAX_WRITE_INV_BUFFERS / PAGE_SIZE,
			(long unsigned int)1 * MAX_ALIVE_THREADS * MAX_WRITE_INV_BUFFERS * PAGE_SIZE,
			(long unsigned int)1 * MAX_ALIVE_THREADS * MAX_WRITE_INV_BUFFERS);

	printk("msg: available size %lu = available pages %lu / %lu bytes\n"
			" wrong #[%lu]\n",
			PCN_KMSG_MAX_PAYLOAD_SIZE,
			(PCN_KMSG_MAX_PAYLOAD_SIZE / PAGE_SIZE),
			(PCN_KMSG_MAX_PAYLOAD_SIZE / PAGE_SIZE) * PAGE_SIZE,
			(((PCN_KMSG_MAX_PAYLOAD_SIZE / PAGE_SIZE) * PAGE_SIZE) /
												sizeof(unsigned long)));

	return 0;
}
