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

#define POPCORN_BARRIER 1
#define INVOKE_INV 0
#define REENTRY_BEGIN_DISABLE 1 // 0: repeat show 1: once
bool batch = true;
//bool batch = false;
static int violation = 0;

//#include "process_server.h"
//#include "vma_server.h"
//#include "page_server.h"
//#include "util.h"
unsigned long long system_tso_wr_cnt = 0;
unsigned long long system_tso_nobenefit_region_cnt = 0;
spinlock_t tso_lock;

static bool print = false;

/* ugly */
extern void __print_pids(struct remote_context *rc);
extern int sync_server_local_conflictions(struct remote_context *rc);
/* (serial/leader)[Local] -> */
void __find_conflictions(int nid, struct remote_context *rc)
{
    page_merge_request_t *req = pcn_kmsg_get(sizeof(*req));
	struct wait_station *ws = get_wait_station(current);
	int i;
	int iter = 0;
	int sent_cnt = 0;
	int single_sent = 0;
	int total_iter;
	bool zero_case = true;
	int local_wr_cnt;

	req->origin_pid = my_nid;
    req->origin_ws = ws->id;
    req->remote_pid = rc->remote_tgids[nid];

	/* TODO: optimizations
	 * 1. construct req->addrs[]
	 *		1.1 hastable local addrs
	 */
	local_wr_cnt = sync_server_local_conflictions(rc);

	// move result to the msg
	// optmization: remove this copy by moving into the func()
	/* dbg */
	//rc->inv_addrs[rc->lconf_cnt];
	if (rc->lconf_cnt)
		PCNPRINTK_ERR("local_conflict_addr_cnt %d\n", rc->lconf_cnt);
	BUG_ON(rc->lconf_cnt > MAX_ALIVE_THREADS * MAX_WRITE_INV_BUFFERS);

	/* scatters - send */
	/* even send when 0 */
	if (local_wr_cnt == 0) {
		iter = 0; /* total == 0 iter == 0 */
		total_iter = 0;
	} else {
		iter++; /* total > 0 iter start from 1 */
		total_iter = (local_wr_cnt / MAX_WRITE_INV_BUFFERS) + 1;
		zero_case = false;
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
		/* can mean */

		/* read from garry */
		memcpy(req->addrs, rc->inv_addrs + sent_cnt,
							sizeof(*req->addrs) * single_sent);

		/* optimize - send_size = sizeof(*req) -
		 *							sizeof(req->addrs) +
		 *							(sizeof(*req->addrs) * single_sent)
		 */
		/* TODO: potential BUG. better to use pending */
		printk("single_sent %d / sent_cnt %d / local_wr_cnt %d %d/%d ->\n",
				single_sent, sent_cnt, local_wr_cnt, iter, total_iter);
		pcn_kmsg_post(PCN_KMSG_TYPE_PAGE_MERGE_REQUEST, nid, req,
								sizeof(*req) - sizeof(req->addrs) +
								(sizeof(*req->addrs) * single_sent));
								//sizeof(*req));

		iter++;
		sent_cnt += single_sent;

		if (zero_case)
			break;
	} while (sent_cnt < local_wr_cnt);

	/* optimization: these sends can be parallel */
	/* send even no diff */
    //pcn_kmsg_post(PCN_KMSG_TYPE_PAGE_MERGE_REQUEST, nid, req, sizeof(*req));

	/* clean all for each run */
	for (i = 0; i < local_wr_cnt; i++)
		rc->inv_addrs[i] = 0;
	local_wr_cnt = 0;

	wait_at_station(ws); // change to consider iters
}

bool find_collision_inv(page_merge_request_t *req, page_merge_response_t *res)
{
	int scatters = 0;
	struct task_struct *tsk = __get_task_struct(req->remote_pid);
	struct remote_context *rc = get_task_remote(tsk);
	BUG_ON(!rc || !tsk);


	// TODO
	// 1. no collision == inv(both)
	// 2. collision = generate diffs + inv(if_remote) (need_merge=true)
	//    				-> res->diffs = xxx;


	// find out address first
	// clear at this point

    put_task_struct(tsk);
	__put_task_remote(rc);
	return scatters;
}

/* -> [Remote] */
static void process_page_merge_request(struct work_struct *work)
{
	START_KMSG_WORK(page_merge_request_t, req, work);
    page_merge_response_t *res = pcn_kmsg_get(sizeof(*res));

	/* put more handshake info to detect skew cases */
	//req->begin =
	//req->fence =
	//req->end =
	// handshak remote status

	res->scatters = find_collision_inv(req, res);

//	if (res->scatters)
//		BUG_ON(res->diffs); // hard to detect

	res->origin_pid = req->origin_pid;
    res->origin_ws = req->origin_ws;
    //res->remote_pid = req->remote_pid;

	/* not only 1 page man... 32k-4 8-1 = 7diffs......*/
//	res->scatters = total;
//	res->merge_id = iter_num;

	res->wr_cnt = req->wr_cnt;
	res->iter = req->iter;
	res->total_iter = req->total_iter;

    pcn_kmsg_post(PCN_KMSG_TYPE_PAGE_MERGE_RESPONSE,
				PCN_KMSG_FROM_NID(req), res, sizeof(*res));
    END_KMSG_WORK(req);
}

/* -> back[Local] */
static int handle_page_merge_response(struct pcn_kmsg_message *msg)
{
    page_merge_response_t *res = (page_merge_response_t *)msg;
    struct wait_station *ws = wait_station(res->origin_ws);

//	if (!res->scatters) /* no need to merge */
//		goto done;

	//res->wr_cnt = req->wr_cnt;
	//res->iter = req->iter;
	//res->total_iter = req->total_iter;

	/* TODO:
	 * 1. fixup
	 * 2. sync with remote
	 */

//done:
	/* TODO: potential BUG. better to use pending */
	if (res->iter == res->total_iter)
		complete(&ws->pendings);

    pcn_kmsg_done(res);
    return 0;
}

/*
 * put rc and pirnk outsite !!!!!!!!!!1
 * mor general
 */
#if 0
static bool __popcorn_barrier(struct remote_context *rc, int a)
{
	bool leader = false;
	int left_t = atomic_dec_return(&rc->barrier);
	BUG_ON(rc->threads_cnt > 96);

	if (left_t == 0) {
		leader = true;
		//SYNCPRINTK("[*][%d] left_t %d\n", current->pid, left_t);

		printk("=== +[*%d] BARRIER %5s! line %d mb %lu b %lu f %lu t %d "
				"Hand Shake done. ===\n",
				current->pid, "BEGIN", a, current->begin_m_cnt,
				current->begin_cnt, current->tso_fence_cnt, rc->threads_cnt);

		//__find_conflictions(!my_nid);
		if (my_nid == 0)
			__find_conflictions(1, rc);
		else
			__find_conflictions(0, rc);

		/* Race Consition !!!!! wait all are in the wq */
		while (atomic_read(&rc->pendings) != rc->threads_cnt - 1)
			io_schedule(); // seems important to yield the atomic op bus in VM

		atomic_inc(&rc->pendings);

		atomic_set(&rc->barrier, rc->threads_cnt);
///////////////////////////////////////////////////
		wake_up_all(&rc->waits);
///////////////////////////////////////////////////
//		complete_all(&rc->comp);
//		init_completion(&rc->comp); // make sure
///////////////////////////////////////////////////

		atomic_dec(&rc->pendings);
		//SYNCPRINTK("[*][%d] done\n", current->pid);
	} else {
		DEFINE_WAIT(wait);
		DEFINE_WAIT(wait_end);
		//SYNCPRINTK("[ ][%d] left_t %d\n", current->pid, left_t);
///////////////////////////////////////////////////
        atomic_inc(&rc->pendings);
		prepare_to_wait_exclusive(&rc->waits, &wait, TASK_UNINTERRUPTIBLE);
		io_schedule();
		finish_wait(&rc->waits, &wait);
///////////////////////////////////////////////////
//		wait_for_completion(&rc->comp);
///////////////////////////////////////////////////

		atomic_dec(&rc->pendings);
	}

	return leader;
}
#endif

static bool __popcorn_barrier_begin(struct remote_context *rc, int a)
{
	bool leader = false;
	int left_t = atomic_dec_return(&rc->barrier);
	BUG_ON(rc->threads_cnt > 96);

	if (left_t == 0) {
		leader = true;
		printk("=== +[*%d] BARRIER %5s! line %d mb %lu b %lu f %lu t %d "
				"Hand Shake done. ===\n",
				current->pid, "BEGIN", a, current->begin_m_cnt,
				current->begin_cnt, current->tso_fence_cnt, rc->threads_cnt);
	}

	return leader;
}

static void __popcorn_barrier_end(struct remote_context *rc, int a, bool leader)
{
	if (leader) {
		printk("=== -[*%d] BARRIER %5s! line %d mb %lu b %lu f %lu t %d "
				"Hand Shake done. ===\n\n",
				current->pid, "END", a, current->begin_m_cnt,
				current->begin_cnt, current->tso_fence_cnt, rc->threads_cnt);

		/* Race Consition !!!!! wait all are in the wq */
		while (atomic_read(&rc->pendings) != rc->threads_cnt - 1)
			io_schedule(); // seems important to yield the atomic op bus in VM

		atomic_inc(&rc->pendings);
		atomic_set(&rc->barrier, rc->threads_cnt);
		wake_up_all(&rc->waits);
		atomic_dec(&rc->pendings);
		//SYNCPRINTK("[*][%d] done\n", current->pid);
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

void collect_tso_wr(void)
{
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
	int i;
	bool leader = false;
	unsigned long inv_cnt; // TODO: change name
	struct remote_context *rc = get_task_remote(current);
	SYNCPRINTK("\t(maybe implicit) [%d] %s():\n", current->pid, __func__);

	if (!current->tso_region) {
		//PCNPRINTK_ERR("[%d] BUG tso_region order violation when \"unlock\"\n",
		//														current->pid);
		goto out;
	}

	if (!current->tso_wr_cnt) {
			//|| !current->tso_wx_cnt)
		//PCNPRINTK_ERR("[%d] WARNNING no benefits here\n", current->pid);
		current->tso_nobenefit_region_cnt++;
	}

#if POPCORN_BARRIER
	//leader = __popcorn_barrier(rc, a);
	leader = __popcorn_barrier_begin(rc, a);

	if (leader) {
		if (my_nid == 0)
			__find_conflictions(1, rc);
		else
			__find_conflictions(0, rc);
	}

	__popcorn_barrier_end(rc, a, leader);
#endif

	current->tso_fence_cnt++;

	//current->tso_region_cnt++;
	//SYNCPRINTK("[%d] %s(): #%llu tso_wr %llu/%llu\n", current->pid, __func__,
	//					current->tso_region_cnt, current->tso_wr_cnt,
	//					current->accu_tso_wr_cnt);

	/* check buffer and invalidate all */
	if (current->tso_wr_cnt > MAX_WRITE_INV_BUFFERS) {
		BUG(); /* should stop in runtime, not here */
		//inv_cnt = MAX_WRITE_INV_BUFFERS; /* suppresed performance */
	} else {
		inv_cnt = current->tso_wr_cnt;
	}

	if (!batch) {
		BUG_ON("Not support: wanna have clean code base");
#if 0
		for (i = 0; i < inv_cnt; i++) { /* deterministic loop */
			unsigned long addr = current->buffer_inv_addrs[i];
			int peers;
			unsigned long *pi;
			unsigned long offset;
			struct page *pip = __get_page_info_page(current->mm, addr, &offset);
			int except_nid = my_nid;

			// send inv (addr)
			if (my_nid == 0) { /* 1. first deal w/ origin only*/
				int nid;
				struct wait_station *ws = get_wait_station(current);
				SYNCPRINTK("[%d] fixing %lx \n", current->pid, addr);
				BUG_ON(!addr);

				if (!pip) BUG(); //return; /* skip claiming non-distributed page */
				pi = (unsigned long *)kmap(pip) + offset;
				peers = bitmap_weight(pi, MAX_POPCORN_NODES);
				if (!peers) {
					kunmap(pip);
					BUG(); /* This page is not distributed */
				}
				SYNCPRINTK("[%d] peers original %d \n", current->pid, peers);
				//BUG_ON(!test_bit(except_nid, pi)); /* we two should have, so don't inv */
				//peers--;	/* exclude except_nid from peers */ // two node case: node true

				/* I have page && falut from other */
				if (test_bit(my_nid, pi) && except_nid != my_nid) peers--;

				SYNCPRINTK("[%d] peers real %d \n", current->pid, peers);
				/* peers is not important here, but why sometimes =1(problem), =2? */

				// 1. don't send at page_server.c // send here
				//__claim_local_page(current, addr, my_nid);
				for_each_set_bit(nid, pi, MAX_POPCORN_NODES) {
					if (nid == my_nid) continue;

					BUG_ON(nid != 1);
					clear_bit(nid, pi);
					SYNCPRINTK("[%d] revoking->@%d addrs[0]=%lx "
								"tso_wr_cnt %llu tso_nobenefit_region_cnt %llu\n",
								current->pid, nid, addr,
								current->tso_wr_cnt,
								current->tso_nobenefit_region_cnt);

					__revoke_page_ownership(current, nid,
										rc->remote_tgids[nid], addr, ws->id);
											/* ask remote worker */
					wait_at_station(ws); /* temporally solution for sometimes
								peer = 1, then skip revoke and wait forever */
				}
				SYNCPRINTK("[%d] fixed %lx \n", current->pid, addr);

				kunmap(pip);
			} else {
				/* TODO: implement since remote cannot issue inv
									(check __claim_local_page) */
				// 0. inplement inv first
				// 1. 2.
			}
			//TODO clearcurrent->buffer_inv_addrs
		}
#endif
	} else { // batch
		if (inv_cnt) {
			// 2. batch: PCN_KMSG_TYPE_PAGE_INVALIDATE_BATCH_REQUEST
			int nid = -1, j;
#if INVOKE_INV
			unsigned long *addrs;
#endif

			if (my_nid == 0)
				nid = 1;
			else if (my_nid == 1)
				nid = 0;
#if INVOKE_INV
			addrs = current->buffer_inv_addrs;
			SYNCPRINTK("[%d] revoking->@%d addrs[0]=%lx tso_wr_cnt %llu\n",
							current->pid, nid, addrs[0], current->tso_wr_cnt);

			// lock? befor calling this function? check claim_
			remote_revoke_page_ownerships(current, nid, /* 2-node assumption */
									rc->remote_tgids[nid], addrs, inv_cnt);

			SYNCPRINTK("[%d] revoking done  addrs[0]=%lx tso_wr_cnt %llu\n",
								current->pid, addrs[0], current->tso_wr_cnt);
#endif
			/* important: makke sure the location */
			for (j = 0; j < inv_cnt; j++)
				current->buffer_inv_addrs[i] = 0;
		}
	}

	/* DEBUG section!!!!!!!! supper bad for performance */
#if 0
	leader = false;
	leader = atomic_dec_and_test(&rc->barrier_end);
	if (leader) {
		printk("=== -[%d] BARRIER %5s! "
				"line %d mb %lu b %lu f %lu t %d ===\n",
				current->pid, "END", a, current->begin_m_cnt,
				current->begin_cnt, current->tso_fence_cnt,
				rc->threads_cnt);

		atomic_set(&rc->barrier_end, rc->threads_cnt);
		complete_all(&rc->comp_end);
	} else {
		wait_for_completion(&rc->comp_end);
	}
#endif
	/* DEBUG section!!!!!!!! supper bad for performance */

	current->tso_wr_cnt = 0;
	current->tso_wx_cnt = 0;
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
	SYNCPRINTK("[%d] %s(): %lu\n", current->pid,
				__func__, current->begin_cnt++);

    //current->tso_region_id = a; // don't uncomment for now
    trace_tso(my_nid, current->pid, a, 'b');

	return 0;
}

SYSCALL_DEFINE2(popcorn_tso_fence, int, a, void __user *, b)
{
	SYNCPRINTK("[%d] %s(): id %d\n", current->pid, __func__, a);
    trace_tso(my_nid, current->pid, a, 'f');
	return __popcorn_tso_fence(a, b);
}

SYSCALL_DEFINE2(popcorn_tso_end, int, a, void __user *, b)
{
	SYNCPRINTK("[%d] %s(): id %d\n", current->pid, __func__, a);
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
	SYNCPRINTK("[%d] %s(): id %lu\n", current->pid,
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
	SYNCPRINTK("[%d] %s(): %lu\n", current->pid,
				__func__, current->begin_m_cnt++);
	return 0;
}

SYSCALL_DEFINE2(popcorn_tso_fence_manual, int, a, void __user *, b)
{
	SYNCPRINTK("[%d] %s():\n", current->pid, __func__);
	return __popcorn_tso_fence(a, b);
}

SYSCALL_DEFINE2(popcorn_tso_end_manual, int, a, void __user *, b)
{
	// TODO: check remaining like _begin?
	SYNCPRINTK("[%d] %s():\n", current->pid, __func__);
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

	//sema_init(&rc->threads_sem, rc->threads_cnt);
	//up(&rc->threads_sem);
	//down(&rc->threads_sem);

	REGISTER_KMSG_WQ_HANDLER(
            PCN_KMSG_TYPE_PAGE_MERGE_REQUEST, page_merge_request);
    REGISTER_KMSG_HANDLER(
            PCN_KMSG_TYPE_PAGE_MERGE_RESPONSE, page_merge_response);

	/* dbg */
	printk("8* %d * %lu = [%lu]/ PAGE = [%lu] pgs\n",
			MAX_ALIVE_THREADS, MAX_WRITE_INV_BUFFERS,
			8 * MAX_ALIVE_THREADS * MAX_WRITE_INV_BUFFERS,
			8 * MAX_ALIVE_THREADS * MAX_WRITE_INV_BUFFERS / PAGE_SIZE);

	printk("msg: available size %lu = available pages %lu / %lu bytes\n"
			" wrong #[%lu]\n",
			PCN_KMSG_MAX_PAYLOAD_SIZE,
			(PCN_KMSG_MAX_PAYLOAD_SIZE / PAGE_SIZE),
			(PCN_KMSG_MAX_PAYLOAD_SIZE / PAGE_SIZE) * PAGE_SIZE,
			(((PCN_KMSG_MAX_PAYLOAD_SIZE / PAGE_SIZE) * PAGE_SIZE) /
												sizeof(unsigned long)));

	return 0;
}
