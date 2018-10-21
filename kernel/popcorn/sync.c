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
#include "sync.h"

#include "trace_events.h"
#include <linux/delay.h>
#include <linux/mm.h>
#include <popcorn/sync.h>
#include <linux/hashtable.h>

#define POPCORN_TSO_BARRIER 1
#define REENTRY_BEGIN_DISABLE 0 // 0: repeat show 1: once

#define PERF_DBG 0 /* sensitive */

#define BARRIER_INFO 1
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

#define BUF_DBG 0
#if BUF_DBG
#define SYNCPRINTK3(...) printk(KERN_INFO __VA_ARGS__)
#else
#define SYNCPRINTK3(...)
#endif

#define MSG_DBG 0 /* TODO */
#if MSG_DBG
#define _MSGPRINTK(...) printk(KERN_INFO __VA_ARGS__)
#else
#define _MSGPRINTK(...)
#endif

#define RGN_DEBUG_THIS 1
#if RGN_DEBUG_THIS
#define RGNPRINTK(...) printk(KERN_INFO __VA_ARGS__)
#else
#define RGNPRINTK(...)
#endif

#define DEVELOPE_DEBUG 1
#if DEVELOPE_DEBUG
#define DVLPRINTK(...) printk(KERN_INFO __VA_ARGS__)
#else
#define DVLPRINTK(...)
#endif

/* rcsi meta/mem hash lock */
#define RCSI_HASH_BITS 10
DEFINE_HASHTABLE(rcsi_hash, RCSI_HASH_BITS);
DEFINE_SPINLOCK(rcsi_hash_lock); //TODO extern in sync.h //no need

unsigned long long system_tso_wr_cnt = 0; // not used
unsigned long long system_tso_nobenefit_region_cnt = 0; // not used
spinlock_t tso_lock;

/* Violation section detecting */
static bool print = false;
static bool print_end = false;

static int violation_begin = 0;
static int violation_end = 0;

/****************************
 *	memory pool for hashing
 * bitm - maintain rcsi_wor pool (doens't care which kmem)
 */
struct rcsi_work {
	struct hlist_node hentry;
	unsigned long addr;			/* fault address */
	char *paddr;				/* SI page */
	int id;						/* id in pool */
} __attribute__((aligned(64)));

#define MAX_PAGE_ODER (MAX_ORDER - 1) /* 1 segment chunks */
#define MAX_PAGE_CHUNKS (1 << MAX_PAGE_ODER) /* 1 segment chunks */
#define MAX_PAGE_CHUNK_SIZE (PAGE_SIZE * MAX_PAGE_CHUNKS) /* 1 segment size */

#define RCSI_POOL_SLOTS (MAX_WRITE_INV_BUFFERS * MAX_ALIVE_THREADS) /* all */
#define RCSI_MEM_POOL_SEGMENTS (((RCSI_POOL_SLOTS - 1) / MAX_PAGE_CHUNKS) + 1) /* mem */

#define MAC_RCSI_POOL_SEGMENTS 100
struct rcsi_work *rcsi_work_pool[MAC_RCSI_POOL_SEGMENTS]; /* meta depents on sizeof(struct rcsi_work). Fix size to make sure it will not overflow */
static DECLARE_BITMAP(rcsi_bmap, RCSI_POOL_SLOTS) = { 0 };
static DEFINE_SPINLOCK(rcsi_bmap_lock);
char *rcsi_mem_pool_segments[RCSI_MEM_POOL_SEGMENTS]; /* for freeing */

/* info for rcsi meta segments */
int rcsi_meta_total_size = -1;
//int MAX_PAGE_CHUNK_SIZE = MAX_PAGE_CHUNK_SIZE;
int rcsi_chunks = -1;

struct rcsi_work *__rcsi_hash(unsigned long addr)
{
	struct rcsi_work *rcsi_w;
	hash_for_each_possible(rcsi_hash, rcsi_w, hentry, addr)
		if (rcsi_w->addr == addr)
			return rcsi_w;
	return NULL;
}

struct rcsi_work *__id_to_rcsi_work(int id)
{
	int size, seg_id, reminder;

	size = id * sizeof(**rcsi_work_pool);
	seg_id = size / MAX_PAGE_CHUNK_SIZE;
	reminder = (size % MAX_PAGE_CHUNK_SIZE) / sizeof(**rcsi_work_pool);
	//printk("rcsi: id %d size %d seg_id %d reminder %d\n", id, size, seg_id, reminder);
	return rcsi_work_pool[seg_id] + reminder;
}

struct rcsi_work *__get_rcsi_work(void)
{
	int id;
	spin_lock(&rcsi_bmap_lock);
	id = find_first_zero_bit(rcsi_bmap, RCSI_POOL_SLOTS);
	set_bit(id, rcsi_bmap);
	spin_unlock(&rcsi_bmap_lock);

	return __id_to_rcsi_work(id);
}

/* WHO to clean the kmem? */
void __put_rcsi_work(struct rcsi_work *rcsi_w)
{
    int id = rcsi_w->id;
	spin_lock(&rcsi_bmap_lock);
    BUG_ON(!test_bit(id, rcsi_bmap));
    clear_bit(id, rcsi_bmap);
	spin_unlock(&rcsi_bmap_lock);
}

/************
 *  TSO
 */
extern void maintain_origin_table(unsigned long target_addr);
void tso_wr_inc(struct vm_area_struct *vma, unsigned long addr, struct page *page, spinlock_t *ptl)
{
	void *paddr = kmap(page);
	int ivn_cnt_tmp;
	struct remote_context *rc = current->mm->remote;
#if HASH_GLOBAL
	struct rcsi_work *found, *rcsi_w_new;
#endif

#ifdef CONFIG_POPCORN_CHECK_SANITY
	BUG_ON(!rc);
	BUG_ON(!paddr);
#endif

#if !GLOBAL /* implementation - local */
	current->buffer_inv_addrs[current->tso_wr_cnt] = addr;
	current->tso_wr_cnt++;
#else /* implementation - global */
#if HASH_GLOBAL
	rcsi_w_new = __get_rcsi_work();

	spin_lock(&rcsi_hash_lock);
	found = __rcsi_hash(addr);
	if (likely(!found))
		hash_add(rcsi_hash, &rcsi_w_new->hentry, addr);
	spin_unlock(&rcsi_hash_lock);

	if (likely(!found)) { /* No local collision */
		BUG_ON(rcsi_w_new->addr);
		rcsi_w_new->addr = addr;
		if (!NOCOPY_NODE) { /* check clean? */
			memcpy(rcsi_w_new->paddr, paddr, PAGE_SIZE);
		}

		/* TODO: atomic 1 line - start */
		spin_lock(&rc->inv_lock);
		ivn_cnt_tmp = rc->inv_cnt++;
		spin_unlock(&rc->inv_lock);
		BUG_ON(rc->inv_addrs[ivn_cnt_tmp]);
		rc->inv_addrs[ivn_cnt_tmp] = addr; /* cnt to know max inv_addrs */
		/* TODO: atomic 1 line - end */
#if BUF_DBG
		SYNCPRINTK("[%d/%d] %s buf 0x%lx ins %lx %d\n", current->pid,
						ivn_cnt_tmp,
						my_nid == 0 ? "origin" : "remote",  addr,
						instruction_pointer(current_pt_regs()),
						rcsi_w_new->id);
#endif
		/* ownership update */
		//spin_lock(ptl); // TODO: check TSO_ORIGIN/REMOTE flow
		maintain_origin_table(addr);
		//spin_unlock(ptl); //
	} else {
		printk(KERN_ERR "local collision\n");
		__put_rcsi_work(rcsi_w_new);
	}
#else
    spin_lock(&rc->inv_lock);
	ivn_cnt_tmp = rc->inv_cnt++; /* TODO: atomic 1 line */
    spin_unlock(&rc->inv_lock);

	//BUG_ON(rc->inv_addrs[ivn_cnt_tmp]);
	rc->inv_addrs[ivn_cnt_tmp] = addr;
	if (!NOCOPY_NODE) {
		BUG_ON(*(rc->inv_pages + (ivn_cnt_tmp * PAGE_SIZE))); /* TODO: BUG: WHY? */
		memcpy(rc->inv_pages + (ivn_cnt_tmp * PAGE_SIZE), paddr, PAGE_SIZE);
		//copy_from_user_page(vma, page, addr,
		//		rc->inv_pages + (ivn_cnt_tmp * PAGE_SIZE), paddr, PAGE_SIZE);
	}
#endif
#endif

	kunmap(page);

#if BUF_DBG
//	SYNCPRINTK("[%d/%d] %s buf 0x%lx ins %lx\n", current->pid, ivn_cnt_tmp,
//					my_nid == 0 ? "origin" : "remote",  addr,
//					instruction_pointer(current_pt_regs()));
#endif
}


/* ugly */
extern void __print_pids(struct remote_context *rc);
extern pte_t *get_pte_at(struct mm_struct *mm, unsigned long addr, pmd_t **ppmd, spinlock_t **ptlp);
/*************************
 * for barriers/fence
 */
void __clean_perthread_inv_buf(int inv_cnt)
{	/* out-dated */
	int j;
	for (j = 0; j < inv_cnt; j++)
		current->buffer_inv_addrs[j] = 0;
}


#if HASH_GLOBAL
/* serial phase */
void __clean_global_rcsi(struct remote_context *rc)
{
	int bkt;
	struct rcsi_work *pos;
	struct hlist_node *tmp;
	/* Attention: lock order */
	/* clean hash table */
	//spin_lock(&rcsi_hash_lock);
	hash_for_each_safe(rcsi_hash, bkt, tmp, pos, hentry) { /* iter objs */
		pos->addr = 0;
		if (!NOCOPY_NODE)
			memset(pos->paddr, 0, PAGE_SIZE); /* TODO: perf critical */
		__put_rcsi_work(pos);
		hlist_del(&pos->hentry);
	}
	BUG_ON(!hash_empty(rcsi_hash));
	//spin_unlock(&rcsi_hash_lock);

	memset(rc->inv_addrs, 0, sizeof(*rc->inv_addrs) * rc->inv_cnt);
//	spin_lock(&rc->inv_lock); // no need
	rc->inv_cnt = 0;
//	spin_unlock(&rc->inv_lock); // no need
}
#endif

#if GLOBAL
#if !HASH_GLOBAL
void __clean_global_inv(struct remote_context *rc)
{
	spin_lock(&rc->inv_lock); // no need
	memset(rc->inv_addrs, 0, sizeof(*rc->inv_addrs) * rc->inv_cnt);
	if (!NOCOPY_NODE)
		memset(rc->inv_pages, 0, rc->inv_cnt * PAGE_SIZE);
	rc->inv_cnt = 0;
	spin_unlock(&rc->inv_lock); // no need
}
#endif
#endif

/* Clean all region-data  */
void __tso_wr_clean_all(struct remote_context *rc)
{
#if GLOBAL
#if HASH_GLOBAL
	__clean_global_rcsi(rc);
#else
	/* clean all for each barrier/fence */
	__clean_global_inv(rc);
#endif
#endif
}


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

extern void sync_clear_page_owner(int nid, struct mm_struct *mm, unsigned long addr);
/******
 * If doing ownership maintain here, more local redunadant hash collision.
 */
void __maintain_ownership_serial(void)
{
	int bkt;
	struct rcsi_work *pos;
	struct hlist_node *tmp;
	hash_for_each_safe(rcsi_hash, bkt, tmp, pos, hentry) { /* iter objs */
		if (my_nid == 0)
			sync_clear_page_owner(1, current->mm, pos->addr);
		else /* The remote:  will deal with ww case later */
			sync_clear_page_owner(0, current->mm, pos->addr);
	}
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
#if HASH_GLOBAL
	/* no collistion */
	local_wr_cnt = rc->inv_cnt;
	rc->sys_rw_cnt += local_wr_cnt;

	/* redo ownership maintaining in serial phase for corner cases */
	__maintain_ownership_serial();
#else
	/* implementation - global */
	local_wr_cnt = sync_server_local_serial_conflictions(rc);
	rc->sys_rw_cnt += local_wr_cnt;
#endif
#endif
	/* RC: make sure list is ready */
	rc->ready = true;
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
		req->fence = current->tso_fence_cnt;
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

	/*	1. wait diffs
		2. wait merge msgs */

//	SYNCPRINTK2("woken up: wait for diffs sync# != -1 (%d)\n", rc->diffs);
//	while (rc->diffs == -1) { io_schedule(); smp_rmb(); } // local not figure out yet. aka remote req not out to me yet
//	SYNCPRINTK2("diffs sync: rc->diffs %d\n", rc->diffs);
	if (my_nid == 0) { /* only origin can sync in this way since origin never sends diff req to remote */
		while (atomic_read(&rc->diffs) != atomic_read(&rc->req_diffs))
			io_schedule();
		SYNCPRINTK2("diffs sync: rc->diffs %d req_diffs %d (origin only)\n",
						atomic_read(&rc->diffs), atomic_read(&rc->req_diffs));
	}
	/* wait untile local apply_diffs are done as well */
	/* TODO: check if the following is required */

	/* find a proper place as long as in leader before END_B */

	/* prevent from double handshake (imbalance) */
	SYNCPRINTK2("mg: wait lr(%d/%d)\n", rc->local_merge_id, rc->remote_merge_id);
	//while(rc->local_merge_id != rc->remote_merge_id) { io_schedule(); smp_mb(); }
	/*BUG() - if spining here, may mean never recv the merge req */
	while(rc->local_merge_id > rc->remote_merge_id) { io_schedule(); smp_mb(); }
	SYNCPRINTK2("mg: pass lr(%d/%d)\n", rc->local_merge_id, rc->remote_merge_id);

	rc->local_done_cnt++;
	smp_wmb();
	SYNCPRINTK2("\t-> handshake lr(%d/%d) diffs %d is_diffed %d remote_done %d\n",
			rc->local_done_cnt, rc->remote_done_cnt,
			atomic_read(&rc->diffs), rc->is_diffed, rc->remote_done);
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
/* Double check this function */
static void __make_diffs_send_back_at_remote(struct task_struct *tsk, struct remote_context *rc, int local_ofs, int nid, unsigned long conflict_addr)
{
	page_diff_apply_request_t *req = kmalloc(sizeof(*req), GFP_KERNEL);
	//page_diff_apply_request_t *req = pcn_kmsg_get(sizeof(*req));
	//unsigned long conflict_addr = rc->inv_addrs[local_ofs]; /* local addr */
	struct vm_area_struct *vma;
    spinlock_t *ptl;
    pmd_t *pmd;
    pte_t *pte;
	struct page *page;
	void *paddr;
#if HASH_GLOBAL
	struct rcsi_work *rcsi_w;
#endif
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
#if HASH_GLOBAL
	/* TODO - mark the function as serial/not - serial I guess */
	//spin_lock(&rcsi_hash_lock);
	rcsi_w = __rcsi_hash(conflict_addr);
	//spin_unlock(&rcsi_hash_lock);
	BUG_ON(!rcsi_w);
	__make_diff(paddr, rcsi_w->paddr, req);
#else
	__make_diff(paddr,  rc->inv_pages + (local_ofs * PAGE_SIZE), req);
#endif
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
/* TODO: BUG in a interrupt context........... kmap_atomic!!!!!! */
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

//	get_page(page); // seems no need
	/* TODO: BUG in a interrupt context........... kmap_atomic!!!!!! */
	paddr = kmap(page);
	//paddr = kmap_atomic(page);
	BUG_ON(!paddr);
	__apply_diff(paddr, req->diff_page);
	//kunmap_atomic(page);
	kunmap(page);
	/* TODO: BUG in a interrupt context........... kmap_atomic!!!!!! */
//	put_page(page); // seems no need

	pte_unmap(pte);

	up_read(&tsk->mm->mmap_sem);
}

/* diff_req -> back[Local] */
extern void sync_set_page_owner(int nid, struct mm_struct *mm, unsigned long addr);
static int handle_page_apply_diff_request(struct pcn_kmsg_message *msg)
{
    page_diff_apply_request_t *req = (page_diff_apply_request_t *)msg;
	struct task_struct *tsk = __get_task_struct(req->origin_pid);
	struct remote_context *rc;
	BUG_ON(!tsk);
	rc = get_task_remote(tsk);
	BUG_ON(!rc);
	BUG_ON(my_nid != NOCOPY_NODE);

//	smp_wmb();

	__do_apply_diff(req, tsk);

	/* ownership */
	// TODO BUG will hang......cause sync proble.........why
	//				sync_set_page_owner(0, tsk->mm, req->diff_addr);
	//				sync_clear_page_owner(1, tsk->mm, req->diff_addr);
	// this msg may/ may not happen. So fix here is not realistic <- WRONG
	// only for ww case // normally inv case has been handled in the first place.
	// this is the 2nd bandadhe for special cases

	atomic_inc(&rc->req_diffs);
	atomic_inc(&rc->sys_ww_cnt);
	DIFFPRINTK("[%d/%5d] locally applying diff 0x%lx\n",
			tsk->pid, atomic_read(&rc->req_diffs), req->diff_addr);

//	rc->is_diffed = true;
//	smp_wmb();

	__put_task_remote(rc);
    put_task_struct(tsk);
	pcn_kmsg_done(req);
	return 0;
}

/* Kernel worker at remote */
/* -> [Remote] (Serial?) */
/* ugly */
int __find_collision_btw_nodes_at_remote(page_merge_request_t *req, struct remote_context *rc, struct task_struct *tsk, int wr_cnt, unsigned long *wr_addrs)
{
	int i, j, conflict_cnt = 0, from_nid = PCN_KMSG_FROM_NID(req);

#ifdef CONFIG_POPCORN_CHECK_SANITY
	if (wr_cnt < 5 && rc->inv_cnt < 5) {
		for (i = 0; i < wr_cnt; i++)
			SYNCPRINTK2("rlist: 0x%lx\n", wr_addrs[i]);

		for (i = 0; i < rc->inv_cnt; i++)
			SYNCPRINTK2("llist: 0x%lx\n", rc->inv_addrs[i]);
	}
#endif

#if GLOBAL
	for (i = 0; i < wr_cnt; i++) {
		/* If no collision, inv.
		 * If collision, if !NOCOPY_NODE, generate diffs + inv + send merge reeq */
		unsigned long req_addr = wr_addrs[i];
		int ret;
		bool conflict = false;
#if HASH_GLOBAL
		struct rcsi_work *rcsi_w = __rcsi_hash(req_addr); /* TODO lock? !think so*/
		if (rcsi_w)
			conflict = true;
#else
		for (j = 0; j < rc->inv_cnt; j++) {
			unsigned long addr = rc->inv_addrs[j];
			if (req_addr == addr) {
				conflict = true;
				break;
			}
		}

#endif
		BUG_ON(!req_addr);
		if (!conflict) {
			/* optimize: delay */
			SYNCPRINTK3("[ar/%d/%3d] %3s 0x%lx\n", my_nid, i, "inv", req_addr);
			ret = do_locally_inv_page(tsk, req_addr);
			if (ret)
				BUG();
		} else { /* both will detect the same conflict addrs */
			conflict_cnt++; /* maintain meta to sync */
			SYNCPRINTK3("[ar/%d/%3d] %3s 0x%lx\n", my_nid, i, "ww", req_addr);
			if (my_nid != NOCOPY_NODE) {
				/* optimize: batch */
				ret = do_locally_inv_page(tsk, req_addr);
				if (ret)
					BUG();
				__make_diffs_send_back_at_remote(tsk, rc, j, from_nid, req_addr);
			}
#if 1
			else { /* NOCOPY_NODE: 0 */
				if (my_nid == 0) { /* only origin maintain pip */
					/* Not True */

					/* TODO try to make sure the ownership again */
					// do_locally_own_page()
					sync_set_page_owner(0, tsk->mm, req_addr);
					sync_clear_page_owner(1, tsk->mm, req_addr);

					/* or can do it in handle_page_apply_diff_request() */
				}
			}
#endif
		}
	}
#else
	BUG_ON("Not supprot for PER-THREAD list");
#endif
	/* cross check */
	rc->remote_sys_ww_cnt += conflict_cnt;
	atomic_add(conflict_cnt, &rc->diffs);
	smp_wmb();
	return conflict_cnt;
}

void __wait_last_reset_done(struct remote_context *rc)
{
	SYNCPRINTK2("L:%d R:%d\n",
			atomic_read(&rc->per_barrier_reset_done), rc->remote_done_cnt);
	/* wait real clean to catch up hand shake, If I don't proceed, handshke will not proceed, rc->remote_done_cnt should not proceed as well. */
	while (atomic_read(&rc->per_barrier_reset_done) != rc->remote_done_cnt)
		io_schedule();
	/* for race condition, prevent from overwritten */ /* This may be a straggler case PERF */ /* no sleep may casue a 100% kernel spining (bus)*/
}

void __wait_local_list_ready(struct remote_context *rc, int wr_cnt)
{
	BARRMPRINTK("prepare to find conflict at remote req->wr_cnt %d (wait rc->ready)\n", wr_cnt);
	/* RC: make sure global list is ready - wait on cache line */
	while(!rc->ready) { io_schedule(); smp_rmb(); }
	/* TODO bug spinging on bare now */
	BARRMPRINTK("find conflict at remote req->wr_cnt %d (barrier)\n", wr_cnt);
}

/* -> [Remote](multiple) */
static void process_page_merge_request(struct work_struct *work)
{
	START_KMSG_WORK(page_merge_request_t, req, work);
    //page_merge_response_t *res = pcn_kmsg_get(sizeof(*res));
    page_merge_response_t *res = kmalloc(sizeof(*res), GFP_KERNEL);
	struct task_struct *tsk = __get_task_struct(req->remote_pid);
	struct remote_context *rc;
	int conflict_cnt;
	int wr_cnt = req->wr_cnt;
	unsigned long *wr_addrs = req->addrs;
	BUG_ON(!tsk);
	rc = get_task_remote(tsk);
	BUG_ON(!res|| !rc);
	SYNCPRINTK2("\t\t<- MERGE request [%d/%d]\n", req->iter, req->total_iter);

	/* put more handshake info to detect skew cases */
	//req->begin =
//	current->begin_m_cnt
//	current->begin_cnt
	//req->fence = current->tso_fence_cnt;
	rc->remote_fence = req->fence;
	//BUG_ON(rc->fence != current->tso_fence_cnt); // too early
	//req->end =
	//mb %lu b %lu f %lu
	// handshak remote status
	rc->remote_merge_id = req->merge_id;
	smp_mb();
	SYNCPRINTK2("mg: r lr(%d/%d)\n", rc->local_merge_id, rc->remote_merge_id);


	/* Barriers */
	__wait_last_reset_done(rc); /* not going to work - instead use begin */
	__wait_local_list_ready(rc, wr_cnt);

	conflict_cnt = __find_collision_btw_nodes_at_remote(req, rc, tsk, wr_cnt, wr_addrs);

	BARRMPRINTK("\t\t{{{ conflict_cnt at remote %d/%d }}}\n",
									conflict_cnt, wr_cnt);

	res->origin_pid = req->origin_pid;
    res->origin_ws = req->origin_ws;
    //res->remote_pid = req->remote_pid;

	/* not only 1 page man... 32k-4 8-1 = 7diffs......*/
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
			atomic_read(&rc->diffs), rc->is_diffed, rc->remote_done);

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

/* (serial) - clean per barrier data in rc */
static void __per_barrier_reset(struct remote_context *rc)
{
	rc->ready = false; /* sync */
	rc->remote_done = false;
	atomic_set(&rc->diffs, 0); /* race condition */ /* async + multiple */
	atomic_set(&rc->req_diffs, 0);

	__tso_wr_clean_all(rc);
	atomic_inc(&rc->per_barrier_reset_done); /* >0: done , 0: !done*/
	smp_wmb();
}

/*
 * put rc and pirnk outsite !!!!!!!!!!1
 * be mor general
 */
static bool __popcorn_barrier_begin(struct remote_context *rc, int id)
{
	bool leader = false;
	int left_t = atomic_dec_return(&rc->barrier);
	BUG_ON(rc->threads_cnt > 96);
	if (left_t == 0) {
		leader = true;
		BARRPRINTK("=== +++[*%d] BARRIER %5s! line %d mb %lu b %lu f %lu t %d "
				"Hand Shake done. ===\n",
				current->pid, "BEGIN", id, current->begin_m_cnt,
				current->begin_cnt, current->tso_fence_cnt, rc->threads_cnt);
	}

	return leader;
}

static void __popcorn_barrier_end(struct remote_context *rc, int id, bool leader)
{
	if (leader) {
		BARRPRINTK("=== ---[*%d] BARRIER %5s! line %d mb %lu b %lu f %lu t %d "
					"Hand Shake done. ===\n\n",
					current->pid, "END", id, current->begin_m_cnt,
					current->begin_cnt, current->tso_fence_cnt, rc->threads_cnt);

		/* Race Consition !!!!! wait all are in the wq */
		while (atomic_read(&rc->pendings) != rc->threads_cnt - 1)
			io_schedule(); // seems important to yield the atomic op bus in VM

		/* remote also done (handshake) */
		//while (!rc->remote_done)
		SYNCPRINTK2("\t- wait lr(%d/%d) diffs %d is_diffed %d remote_done %d\n",
					rc->local_done_cnt, rc->remote_done_cnt,
					atomic_read(&rc->diffs), rc->is_diffed, rc->remote_done);
		while (rc->local_done_cnt > rc->remote_done_cnt) {
			io_schedule(); smp_rmb();
		}
/* at this moment - remote might have sent to me MERGE req */
/* race condition rc-> diffs is 0 but reset by later -1 */
		SYNCPRINTK2("\t- pass lr(%d/%d)\n",
					rc->local_done_cnt, rc->remote_done_cnt);

		// gurdian
		if (rc->local_done_cnt != rc->remote_done_cnt) {
			printk(KERN_ERR "lf: %lu rf: %d\n",
					current->tso_fence_cnt, rc->remote_fence);
		}
		//BUG_ON(rc->remote_fence != current->tso_fence_cnt); /* gurdian false-true */

		__per_barrier_reset(rc); /* CLEAN before waking up followers */
		atomic_inc(&rc->pendings);
		atomic_set(&rc->barrier, rc->threads_cnt);
		wake_up_all(&rc->waits);
		atomic_dec(&rc->pendings);

		BARRPRINTK("=== ---[*%d] BARRIER %5s! line %d mb %lu b %lu f %lu t %d "
				"Hand Shake done. ===\n\n",
				current->pid, "END*", id, current->begin_m_cnt,
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

	printk("[%d]: %s exit sys_rw_cnt %lu "
						"sys_ww_cnt %d (remote side %lu)  "
						"sys_inv_cnt %lu "
						"sys_local_conflict_cnt %lu "
						"violation_begin %d violation_end %d\n",
						tsk->pid, tsk->comm,
						rc->sys_rw_cnt,
						atomic_read(&rc->sys_ww_cnt),
						rc->remote_sys_ww_cnt,
		rc->sys_rw_cnt - atomic_read(&rc->sys_ww_cnt) - rc->remote_sys_ww_cnt,
						rc->sys_local_conflict_cnt,
						violation_begin, violation_end);
	__put_task_remote(rc);
}

void clean_tso_wr(void)
{
	spin_lock(&tso_lock);
	system_tso_wr_cnt = 0;
	system_tso_nobenefit_region_cnt = 0;
	spin_unlock(&tso_lock);
}

static int __popcorn_tso_fence(int id, void __user * file, unsigned long omp_hash, int a, void __user * b)
{
	bool leader = false;
	struct remote_context *rc = get_task_remote(current);
	//RGNPRINTK("\t\t(maybe implicit) [%d] %s(): id %d\n", current->pid, __func__, id);
	if (!current->tso_region) { /* open to detect errors */
		PCNPRINTK_ERR("[%d] BUG fence in a not-tso region "
						"tso_region_id %lu line %d omp_hash 0x%lx - IGNORE\n",
						current->pid, current->tso_region_id, id, omp_hash);
		//PCNPRINTK_ERR("[%d] BUG order violation when "
		//								"\"unlock\"\n", current->pid);
		goto out;
	}

#if POPCORN_TSO_BARRIER
	leader = __popcorn_barrier_begin(rc, id);

	if (leader) {
		if (my_nid == 0)
			__locally_find_conflictions(1, rc);
		else
			__locally_find_conflictions(0, rc);
	}

	__popcorn_barrier_end(rc, id, leader);
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
SYSCALL_DEFINE5(popcorn_tso_begin, int, id, void __user *, file, unsigned long, omp_hash, int, a, void __user *, b)
{
	// TODO: merge to one
	if (current->tso_region || current->tso_wr_cnt || current->tso_wx_cnt) {
		WARN_ON_ONCE("BUG \"tso_begin\" order violation");
		if (!print) {
#if REENTRY_BEGIN_DISABLE
			print = true;
#endif
			PCNPRINTK_ERR("[%d] BUG \"tso_begin\" order violation "
						"region (%s) tso_wr %llu tso_wx %llu line %d\n",
									current->pid, current->tso_region?"O":"X",
									current->tso_wr_cnt, current->tso_wx_cnt, id);
		}
		violation_begin++;
		//__popcorn_tso_fence(id, file, omp_hash, a, b); /* weired case..... but we have to fix NMW */
	}
	current->tso_region = true;
	RGNPRINTK("[%d] %s(): id %d omp_hash 0x%lx cnt %lu\n", current->pid,
				__func__, id, omp_hash, current->begin_cnt++);

    current->tso_region_id = id; // don't uncomment for now
    trace_tso(my_nid, current->pid, id, 'b');

	return 0;
}

SYSCALL_DEFINE5(popcorn_tso_fence, int, id, void __user *, file, unsigned long, omp_hash, int, a, void __user *, b)
{
	RGNPRINTK("[%d] %s(): id %d omp_hash 0x%lx\n", current->pid, __func__, id, omp_hash);
    trace_tso(my_nid, current->pid, id, 'f');
	return __popcorn_tso_fence(id, file, omp_hash, id, b);
}

SYSCALL_DEFINE5(popcorn_tso_end, int, id, void __user *, file, unsigned long, omp_hash, int, a, void __user *, b)
{
	RGNPRINTK("[%d] %s(): id %d omp_hash 0x%lx -> implicit fence\n", current->pid, __func__, id, omp_hash);
	if (!current->tso_region || !current->tso_region_id) {
		WARN_ON_ONCE("BUG \"tso_end\" order violation");
		if (!print_end) {
#if REENTRY_BEGIN_DISABLE
			print_end = true;
#endif
			PCNPRINTK_ERR("[%d] BUG \"tso_end\" order violation "
						"region (%s) tso_region_id %lu line %d omp_hash 0x%lx\n",
									current->pid, current->tso_region?"O":"X",
									current->tso_region_id, id, omp_hash);
		}
		violation_end++;
		//__popcorn_tso_fence(id, file, omp_hash, a, b); /* weired case..... but we have to fix NMW */
	}

	if (!current->tso_region) return 0;
    trace_tso(my_nid, current->pid, id, 'e');
	__popcorn_tso_fence(id, file, omp_hash, a, b);
	current->tso_region = false;
	current->tso_region_id = 0;
	current->tso_region_cnt++;
	return 0;
}

SYSCALL_DEFINE5(popcorn_tso_id, int, id, void __user *, file, unsigned long, omp_hash, int, a, void __user *, b)
{
	if (current->tso_region) {
		// warnning?
	}
	current->tso_region_id = id;
	RGNPRINTK("[%d] %s(): id %lu? omp_hash 0x%lx\n", current->pid,
				__func__, current->tso_region_id, omp_hash);
	return 0;
}

SYSCALL_DEFINE5(popcorn_tso_begin_manual, int, id, void __user *, file, unsigned long, omp_hash, int, a, void __user *, b)
{
	// TODO: merge to one
	if (current->tso_region || current->tso_wr_cnt || current->tso_wx_cnt) {
		WARN_ON_ONCE("BUG \"tso_begin_manual\" order violation");
		if (!print) {
#if REENTRY_BEGIN_DISABLE
			print = true;
#endif
			PCNPRINTK_ERR("[%d] BUG \"tso_begin_manual\" order violation "
							"region (%s) tso_wr %llu tso_wx %llu line %d omp_hash 0x%lx\n",
							current->pid, current->tso_region?"O":"X",
							current->tso_wr_cnt, current->tso_wx_cnt, id, omp_hash);
		}
		violation_begin++;
		//__popcorn_tso_fence(id, file, omp_hash, a, b); /* weired case..... but we have to fix NMW */
	}
	current->tso_region = true;
    current->tso_region_id = id; // don't uncomment for now
	RGNPRINTK("[%d] %s(): omp_hash 0x%lx cnt %lu\n", current->pid,
				__func__, omp_hash, current->begin_m_cnt++);
	return 0;
}

SYSCALL_DEFINE5(popcorn_tso_fence_manual, int, id, void __user *, file, unsigned long, omp_hash, int, a, void __user *, b)
{
	RGNPRINTK("[%d] %s():\n", current->pid, __func__);
	return __popcorn_tso_fence(id, file, omp_hash, a, b);
}

SYSCALL_DEFINE5(popcorn_tso_end_manual, int, id, void __user *, file, unsigned long, omp_hash, int, a, void __user *, b)
{
	RGNPRINTK("[%d] %s(): id %d omp_hash 0x%lx -> implicit fence\n", current->pid, __func__, id, omp_hash);
	if (!current->tso_region || !current->tso_region_id) {
		WARN_ON_ONCE("BUG \"tso_end_manual\" order violation");
		if (!print_end) {
#if REENTRY_BEGIN_DISABLE
			print_end = true;
#endif
			PCNPRINTK_ERR("[%d] BUG \"tso_end_manual\" order violation"
						"region (%s) tso_region_id %lu line %d omp_hash 0x%lx\n",
									current->pid, current->tso_region?"O":"X",
									current->tso_region_id, id, omp_hash);
		}
		violation_end++;
		//__popcorn_tso_fence(id, file, omp_hash, a, b); /* weired case..... but we have to fix NMW */
	}

	__popcorn_tso_fence(id, file, omp_hash, a, b);
	current->tso_region = false;
	current->tso_region_cnt++;
	return 0;
}
#else // CONFIG_POPCORN
SYSCALL_DEFINE5(popcorn_tso_begin, int, id, void __user *, file, unsigned long, omp_hash, iint, a void __user *, b)
{
	PCNPRINTK_ERR("Kernel is not configured to use popcorn\n");
	return -EPERM;
}

SYSCALL_DEFINE5(popcorn_tso_fence, int, id, void __user *, file, unsigned long, omp_hash, iint, a, void __user *, b)
{
	PCNPRINTK_ERR("Kernel is not configured to use popcorn\n");
	return -EPERM;
}

SYSCALL_DEFINE5(popcorn_tso_end, int, id, void __user *, file, unsigned long, omp_hash, iint, a, void __user *, b)
{
	PCNPRINTK_ERR("Kernel is not configured to use popcorn\n");
	return -EPERM;
}

SYSCALL_DEFINE5(popcorn_tso_id, int, id, void __user *, file, unsigned long, omp_hash, iint, a, void __user *, b)
{
	PCNPRINTK_ERR("Kernel is not configured to use popcorn\n");
	return -EPERM;
}


SYSCALL_DEFINE5(popcorn_tso_begin_manual, int, id, void __user *, file, unsigned long, omp_hash, iint, a void __user *, b)
{
	PCNPRINTK_ERR("Kernel is not configured to use popcorn\n");
	return -EPERM;
}

SYSCALL_DEFINE5(popcorn_tso_fence_manual, int, id, void __user *, file, unsigned long, omp_hash, iint, a, void __user *, b)
{
	PCNPRINTK_ERR("Kernel is not configured to use popcorn\n");
	return -EPERM;
}

SYSCALL_DEFINE5(popcorn_tso_end_manual, int, id, void __user *, file, unsigned long, omp_hash, iint, a, void __user *, b)
{
	PCNPRINTK_ERR("Kernel is not configured to use popcorn\n");
	return -EPERM;
}
#endif

void __rcsi_mem_alloc(void)
{
	int i;

	/* memory pool */
	for (i = 0; i < RCSI_MEM_POOL_SEGMENTS; i++) {
		rcsi_mem_pool_segments[i] = kzalloc(sizeof(**rcsi_mem_pool_segments)
								* MAX_PAGE_CHUNK_SIZE, GFP_KERNEL); /* TODO free */
		if (!rcsi_mem_pool_segments[i])
			BUG();
	}

	/* rcsi pool */
	if (sizeof(struct rcsi_work) > 64)
		BUG_ON(printk(KERN_ERR "struct rcsi_work size is > "
								"64 need to change aligned\n"));
	if (PAGE_SIZE % sizeof(struct rcsi_work))
		BUG_ON(printk(KERN_ERR "struct rcsi_work size - "
								"make sure it's divisable by PAGE_SIZE\n"));

	/* update global variables */
	rcsi_meta_total_size = sizeof(**rcsi_work_pool) * RCSI_POOL_SLOTS;
	rcsi_chunks = (rcsi_meta_total_size / MAX_PAGE_CHUNK_SIZE) + 1;


	for (i = 0; i < rcsi_chunks; i++) {
		/* TODO free */
		rcsi_work_pool[i] = kzalloc(MAX_PAGE_CHUNK_SIZE, GFP_KERNEL);
		if (!rcsi_work_pool[i])
			BUG();
	}

	/* match rcsi_work with mem slot */
	for (i = 0; i < RCSI_POOL_SLOTS; i++) {
		struct rcsi_work *rcsi_w = __id_to_rcsi_work(i);
		rcsi_w->addr = 0;
		rcsi_w->id = i;
		rcsi_w->paddr = rcsi_mem_pool_segments[i / MAX_PAGE_CHUNKS] +
									(PAGE_SIZE * (i % MAX_PAGE_CHUNKS));
	}

	/* dbg */
	DVLPRINTK("rcsi: meta/memory pool info:\n");
	DVLPRINTK("\t\t%4s - segs %d slots %lu size %d\n", "rcsi",
				rcsi_chunks, RCSI_POOL_SLOTS, rcsi_meta_total_size);
	DVLPRINTK("\t\t%4s - segs %lu slots %lu size %lu\n", "mem",
		RCSI_MEM_POOL_SEGMENTS, RCSI_POOL_SLOTS, RCSI_POOL_SLOTS * PAGE_SIZE);
}

void __rcsi_hash_init(void)
{
	return;
}

void __rcsi_hash_test1(unsigned long addr)
{
	struct rcsi_work *rcsi_w;
	struct rcsi_work _rcsi_w;
	bool found = false;
	_rcsi_w.addr = addr;
	printk("%s: hash_empty(%s)\n", __func__, hash_empty(rcsi_hash) ? "O" : "X");
	hash_add(rcsi_hash, &_rcsi_w.hentry, _rcsi_w.addr);
	printk("%s: hash_empty(%s)\n", __func__, hash_empty(rcsi_hash) ? "O" : "X");

	hash_for_each_possible(rcsi_hash, rcsi_w, hentry, addr) {
		/* for loop inside a bucket */
		if (rcsi_w->addr == addr) {
			found = true;
			break;
		}
	}
	printk("%s: found(%s) exp(O)\n", __func__, found ? "O": "X");
	printk("%s: hash_empty(%s)\n", __func__, hash_empty(rcsi_hash) ? "O" : "X");
}

void __rcsi_hash_test2(unsigned long addr)
{
	struct rcsi_work *rcsi_w;
	struct hlist_node *tmp;
	bool found = false;
	hash_for_each_possible_safe(rcsi_hash, rcsi_w, tmp, hentry, addr) {
		/* for loop inside a bucket */
		if (rcsi_w->addr == addr) {
			found = true;
			hash_del(&rcsi_w->hentry);
		}
	}
	printk("%s: found(%s) exp(O)\n", __func__, found ? "O": "X");
	printk("%s: hash_empty(%s)\n", __func__, hash_empty(rcsi_hash) ? "O" : "X");

	found = false;
	hash_for_each_possible(rcsi_hash, rcsi_w, hentry, addr) {
		/* inside bucket - iterate the list */
		if (rcsi_w->addr == addr) {
			found = true; // shouldn't entry here twice
		}
	}
	printk("%s: found(%s) exp(X)\n", __func__, found ? "O" : "X");
	printk("%s: hash_empty(%s)\n", __func__, hash_empty(rcsi_hash) ? "O" : "X");
}

struct rcsi_work *__rcsi_hash_test3(void)
{
#define RCSI_CNT 5
	unsigned long addrs[RCSI_CNT] = { 0xad2000, 0xad2000, 0xad3000, 0xad4000, 0xad5000};
	int i;
	struct rcsi_work *_rcsi_w = kmalloc(sizeof(struct rcsi_work) * RCSI_CNT, GFP_KERNEL);
	if (!_rcsi_w) BUG();
	for (i = 0; i < RCSI_CNT; i++) {
		struct rcsi_work *pos = _rcsi_w + i;
		pos->addr = addrs[i];
		printk("_rcsi_w[%d]/pos %p 0x%lx\n", i, pos, pos->addr);
		hash_add(rcsi_hash, &pos->hentry, pos->addr);
	}

	return _rcsi_w;
}

void __rcsi_hash_test4(void)
{
	int bkt;
	struct rcsi_work *pos;
	struct hlist_node *tmp;
	printk("%s: 2. hash_empty(%s)\n", __func__, hash_empty(rcsi_hash) ? "O" : "X");
	hash_for_each_safe(rcsi_hash, bkt, tmp, pos, hentry) { /* iter objs */
		printk("check bkt %d\n", bkt);
		printk("\tkill pos %p\n", pos);
		hlist_del(&pos->hentry);
	}
	printk("%s: 3. hash_empty(%s) exp(O)\n", __func__, hash_empty(rcsi_hash) ? "O" : "X");
}

void __rcsi_hash_test(void) {
	struct rcsi_work *rcsi_w;
	unsigned long addr = 0xad7000;
	__rcsi_hash_test1(addr);
	__rcsi_hash_test2(addr);
	rcsi_w = __rcsi_hash_test3();
	__rcsi_hash_test4();
	kfree(rcsi_w);
}

DEFINE_KMSG_WQ_HANDLER(page_merge_request);
int __init popcorn_sync_init(void)
{
	__rcsi_hash_init();
	__rcsi_hash_test();
	__rcsi_mem_alloc();
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
	DVLPRINTK("si: 8 * %d * %d = [%lu]/ PAGE = [%lu] pgs - "
			"data size [%lu] / PAGE = [%lu] pgs\n",
			(int)MAX_ALIVE_THREADS, (int)MAX_WRITE_INV_BUFFERS,
			(long unsigned int)8 * MAX_ALIVE_THREADS * MAX_WRITE_INV_BUFFERS,
			(long unsigned int)8 * MAX_ALIVE_THREADS * MAX_WRITE_INV_BUFFERS / PAGE_SIZE,
			(long unsigned int)1 * MAX_ALIVE_THREADS * MAX_WRITE_INV_BUFFERS * PAGE_SIZE,
			(long unsigned int)1 * MAX_ALIVE_THREADS * MAX_WRITE_INV_BUFFERS);

	DVLPRINTK("msg: available size %lu = available pages %lu / %lu bytes\n"
			" wrong #[%lu]\n",
			PCN_KMSG_MAX_PAYLOAD_SIZE,
			(PCN_KMSG_MAX_PAYLOAD_SIZE / PAGE_SIZE),
			(PCN_KMSG_MAX_PAYLOAD_SIZE / PAGE_SIZE) * PAGE_SIZE,
			(((PCN_KMSG_MAX_PAYLOAD_SIZE / PAGE_SIZE) * PAGE_SIZE) /
												sizeof(unsigned long)));

	return 0;
}
