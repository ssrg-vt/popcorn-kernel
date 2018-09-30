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

#include "wait_station.h"
#include "page_server.h"
#include "types.h"

#include "trace_events.h"

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

#ifdef CONFIG_POPCORN


int __popcorn_tso_fence(int a, void __user * b)
{
	int i;
	unsigned long tmp; // TODO: change name
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

	current->tso_region_cnt++;
	//SYNCPRINTK("[%d] %s(): #%llu tso_wr %llu/%llu\n", current->pid, __func__,
	//					current->tso_region_cnt, current->tso_wr_cnt,
	//					current->accu_tso_wr_cnt);

	/* check buffer and invalidate all */
	if (current->tso_wr_cnt > MAX_WRITE_INV_BUFFERS)
		tmp = MAX_WRITE_INV_BUFFERS; /* suppresed performance */
	else
		tmp = current->tso_wr_cnt;

	if (!batch) {
		for (i = 0; i < tmp; i++) { /* deterministic loop */
			unsigned long addr = current->buffer_inv_addrs[i];
			int peers;
			unsigned long *pi;
			unsigned long offset;
			struct page *pip = __get_page_info_page(current->mm, addr, &offset);
			int except_nid = my_nid;

			// send inv (addr)
			if (!my_nid) { /* 1. first deal w/ origin only*/
				int nid;
				struct remote_context *rc = get_task_remote(current);
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
				put_task_remote(current);
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
	} else { // batch
		if (tmp) {
			// 2. batch: PCN_KMSG_TYPE_PAGE_INVALIDATE_BATCH_REQUEST
			int nid = -1, j;
			unsigned long *addrs = current->buffer_inv_addrs;
			struct remote_context *rc = get_task_remote(current);
#ifdef CONFIG_POPCORN_CHECK_SANITY
			if (my_nid == 0)
				nid = 1;
			else if (my_nid == 1)
				nid = 0;
#endif
			SYNCPRINTK("[%d] revoking->@%d addrs[0]=%lx tso_wr_cnt %llu\n",
							current->pid, nid, addrs[0], current->tso_wr_cnt);

			// lock? befor calling this function? check claim_
			__revoke_page_ownerships(current, nid, /* 2-node assumption */
									rc->remote_tgids[nid], addrs, tmp);

			put_task_remote(current);
			SYNCPRINTK("[%d] revoking done  addrs[0]=%lx tso_wr_cnt %llu\n",
								current->pid, addrs[0], current->tso_wr_cnt);

			for (j = 0; j < tmp; j++)
				current->buffer_inv_addrs[i] = 0;
		}
	}

	current->tso_wr_cnt = 0;
	current->tso_wx_cnt = 0;
out:
	return 0;
}

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
				__func__, current->begin_m_cnt++);

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

int __init popcorn_sync_init(void)
{
	spin_lock_init(&tso_lock);
	return 0;
}
