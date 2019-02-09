/**
 * @file process_server.c
 *
 * Popcorn Linux thread migration implementation
 * This work was an extension of David Katz MS Thesis, but totally rewritten
 * by Sang-Hoon to support multithread environment.
 *
 * @author Sang-Hoon Kim, SSRG Virginia Tech 2017
 * @author Antonio Barbalace, SSRG Virginia Tech 2014-2016
 * @author Vincent Legout, Sharat Kumar Bath, Ajithchandra Saya, SSRG Virginia Tech 2014-2015
 * @author David Katz, Marina Sadini, SSRG Virginia 2013
 */

#include <linux/sched.h>
#include <linux/threads.h>
#include <linux/slab.h>
#include <linux/kthread.h>
#include <linux/ptrace.h>
#include <linux/mmu_context.h>
#include <linux/fs.h>
#include <linux/futex.h>
#include <linux/highmem.h>

#include <asm/mmu_context.h>
#include <asm/kdebug.h>
#include <asm/uaccess.h>

#include <popcorn/types.h>
#include <popcorn/bundle.h>
#include <popcorn/cpuinfo.h>

#include "types.h"
#include "process_server.h"
#include "vma_server.h"
#include "page_server.h"
#include "wait_station.h"
#include "util.h"
//#include "sync.h"
#include <linux/vmalloc.h>
#include <linux/delay.h>

#define FUTEX_DBG 0
//#define LOCAL_CONFLICT_DBG 0

#define SYNC_DEBUG_THIS 0
#if SYNC_DEBUG_THIS
#define SYNCPRINTK2(...) printk(KERN_INFO __VA_ARGS__)
#else
#define SYNCPRINTK2(...)
#endif

#define BARRIER_INFO_MORE 0
#if BARRIER_INFO_MORE
#define BARRMPRINTK(...) printk(KERN_INFO __VA_ARGS__)
#else
#define BARRMPRINTK(...)
#endif

#define PID_INFO 0
#if PID_INFO
#define PIDPRINTK(...) printk(KERN_INFO __VA_ARGS__)
#else
#define PIDPRINTK(...)
#endif

static struct list_head remote_contexts[2];
static spinlock_t remote_contexts_lock[2];

enum {
	INDEX_OUTBOUND = 0,
	INDEX_INBOUND = 1,
};

/* Hold the correnponding remote_contexts_lock */
static struct remote_context *__lookup_remote_contexts_in(int nid, int tgid)
{
	struct remote_context *rc;

	list_for_each_entry(rc, remote_contexts + INDEX_INBOUND, list) {
		if (rc->remote_tgids[nid] == tgid) {
			return rc;
		}
	}
	return NULL;
}

#define __lock_remote_contexts(index) \
	spin_lock(remote_contexts_lock + index)
#define __lock_remote_contexts_in(nid) \
	__lock_remote_contexts(INDEX_INBOUND)
#define __lock_remote_contexts_out(nid) \
	__lock_remote_contexts(INDEX_OUTBOUND)

#define __unlock_remote_contexts(index) \
	spin_unlock(remote_contexts_lock + index)
#define __unlock_remote_contexts_in(nid) \
	__unlock_remote_contexts(INDEX_INBOUND)
#define __unlock_remote_contexts_out(nid) \
	__unlock_remote_contexts(INDEX_OUTBOUND)

#define __remote_contexts_in() remote_contexts[INDEX_INBOUND]
#define __remote_contexts_out() remote_contexts[INDEX_OUTBOUND]


inline struct remote_context *__get_mm_remote(struct mm_struct *mm)
{
	struct remote_context *rc = mm->remote;
	atomic_inc(&rc->count);
	return rc;
}

struct remote_context *get_task_remote(struct task_struct *tsk)
{
	return __get_mm_remote(tsk->mm);
}

inline bool __put_task_remote(struct remote_context *rc)
{
	if (!atomic_dec_and_test(&rc->count)) return false;

	__lock_remote_contexts(rc->for_remote);
#ifdef CONFIG_POPCORN_CHECK_SANITY
	BUG_ON(atomic_read(&rc->count));
#endif
	list_del(&rc->list);
	__unlock_remote_contexts(rc->for_remote);

	free_remote_context_pages(rc);
#if !HASH_GLOBAL
	vfree(rc->inv_pages);
#endif
	kfree(rc);
	return true;
}

bool put_task_remote(struct task_struct *tsk)
{
	return __put_task_remote(tsk->mm->remote);
}

void free_remote_context(struct remote_context *rc)
{
#ifdef CONFIG_POPCORN_CHECK_SANITY
	BUG_ON(atomic_read(&rc->count) != 1 && atomic_read(&rc->count) != 2);
#endif
	__put_task_remote(rc);
}

static inline void __sync_init(struct remote_context *rc)
{
#if GLOBAL
	/* global */
    spin_lock_init(&rc->inv_lock);
    rc->inv_cnt = 0;		/* per region */
    rc->remote_fence = -1;		/* per region */
    rc->sys_rw_cnt = 0;		/* all rw */
    atomic_set(&rc->sys_ww_cnt, 0);		/* all ww from concurrent msgs */
    rc->remote_sys_ww_cnt = 0;	/* all ww */
    rc->sys_local_conflict_cnt = 0;

    //rc->sys_inv_cnt = 0; // not used = inv_cnt-sys_rw_cnt

	memset(rc->inv_addrs, 0, sizeof(*rc->inv_addrs) *
			MAX_ALIVE_THREADS * MAX_WRITE_INV_BUFFERS);
	//rc->inv_pages = kmalloc(sizeof(*rc->inv_pages) * MAX_ALIVE_THREADS *
	//						MAX_WRITE_INV_BUFFERS * PAGE_SIZE, GFP_KERNEL);
#if !HASH_GLOBAL
	rc->inv_pages = vmalloc(sizeof(*rc->inv_pages) * PAGE_SIZE *
							MAX_WRITE_INV_BUFFERS * MAX_ALIVE_THREADS);
	if (!rc->inv_pages) BUG();
	memset(rc->inv_pages, 0, sizeof(*rc->inv_pages) * PAGE_SIZE *
						MAX_WRITE_INV_BUFFERS * MAX_ALIVE_THREADS);
#endif
#endif
}

static struct remote_context *__alloc_remote_context(int nid, int tgid, bool remote)
{
	struct remote_context *rc = kmalloc(sizeof(*rc), GFP_KERNEL);
	int i;

	if (!rc) return ERR_PTR(-ENOMEM);

	INIT_LIST_HEAD(&rc->list);
	atomic_set(&rc->count, 1); /* Account for mm->remote in a near future */
	rc->mm = NULL;

	rc->tgid = tgid;
	rc->for_remote = remote;

	for (i = 0; i < FAULTS_HASH; i++) {
		INIT_HLIST_HEAD(&rc->faults[i]);
		spin_lock_init(&rc->faults_lock[i]);
	}

	INIT_LIST_HEAD(&rc->vmas);
	spin_lock_init(&rc->vmas_lock);

	rc->stop_remote_worker = false;

	rc->remote_worker = NULL;
	INIT_LIST_HEAD(&rc->remote_works);
	spin_lock_init(&rc->remote_works_lock);
	init_completion(&rc->remote_works_ready);

	memset(rc->remote_tgids, 0x00, sizeof(rc->remote_tgids));

	INIT_RADIX_TREE(&rc->pages, GFP_ATOMIC);

	/* Alive threads */
	rc->threads_cnt = 0;
	rc->out_threads = 0;
	memset(rc->pids, 0, sizeof(*rc->pids) * MAX_ALIVE_THREADS);
	spin_lock_init(&rc->pids_lock);

	/* Barrier - leader/follower */
	init_waitqueue_head(&rc->waits);
	init_waitqueue_head(&rc->waits_end);
	init_completion(&rc->comp);		/* using only one may race condition since the next wait is faster than previous comp */
	init_completion(&rc->comp_end);
	atomic_set(&rc->barrier, rc->threads_cnt);
	atomic_set(&rc->pendings, 0);
	atomic_set(&rc->barrier_end, rc->threads_cnt);

	/* Per barrier data */
	rc->ready = false;
	//atomic_set(&rc->diffs, 0);
	atomic_set(&rc->diffs, 0);
	atomic_set(&rc->per_barrier_reset_done, 0);
	atomic_set(&rc->req_diffs, 0);
	rc->is_diffed = false;
	rc->remote_done = false;
	rc->local_done_cnt = 0;
	rc->remote_done_cnt = 0;
	rc->local_merge_id = 0;
	rc->remote_merge_id = 0;

#if !GLOBAL
	/* serial phase inv/diff */
	int j;
	rc->lconf_cnt = 0; /* local confliction */
	for (i = 0; i < MAX_ALIVE_THREADS; i++) {
		spin_lock_init(&rc->inv_lock_t[i]);
		rc->inv_cnt_t[i] = 0;
		rc->inv_pages_t[i] = kmalloc(sizeof(*rc->inv_pages_t) *
							MAX_WRITE_INV_BUFFERS * PAGE_SIZE, GFP_KERNEL);
		if (!rc->inv_pages_t[i]) BUG();

		for (j = 0;j < MAX_WRITE_INV_BUFFERS; j++)
			rc->time_t[i][j] = 0;
	}

#else

#endif
	/* backup */

	return rc;
}

static void __build_task_comm(char *buffer, char *path)
{
	int i, ch;
	for (i = 0; (ch = *(path++)) != '\0';) {
		if (ch == '/')
			i = 0;
		else if (i < (TASK_COMM_LEN - 1))
			buffer[i++] = ch;
	}
	buffer[i] = '\0';
}

/*
 *
 */
static inline void __check_ofs(int ofs)
{
	if (ofs > MAX_ALIVE_THREADS) BUG();
}

/* rc->pids_lock holded */
int __first_empty_pid_slot(struct remote_context *rc)
{
	int ofs;
again:
	for (ofs = 0; ofs < MAX_ALIVE_THREADS; ofs++) {
		__check_ofs(ofs);
		if (!rc->pids[ofs]) {
			//spin_unlock(&rc->pids_lock);
			return ofs;
		}
	}
	PCNPRINTK_ERR("%s: MAX_ALIVE_THREADS %d\n", __func__, MAX_ALIVE_THREADS);
	PCNPRINTK_ERR("Must succeed: We currently only support up to MAX_ALIVE_THREADS "
													"alive threads on a node");
	BUG();
	//msleep(5000);
	//io_schedule();
	goto again; /* TODO: this is just a workaround..... */
}

/* rc->pids_lock holded */
int __pid_slot(struct remote_context *rc, int pid)
{
	int ofs;
again:
	for (ofs = 0; ofs < MAX_ALIVE_THREADS; ofs++) {
		__check_ofs(ofs);
		if (rc->pids[ofs] == pid) {
			//spin_unlock(&rc->pids_lock);
			return ofs;
		}
	}
	PCNPRINTK_ERR("Must succeed: find %d\n", pid); // TODO: BUG: IT HAPPENS
	BUG();
	//msleep(5000);
	//io_schedule();
	goto again; /* TODO: this is just a workaround..... */
}

/* dbg utility func  */
void __print_pids(struct remote_context *rc)
{
	int ofs;
	//spin_lock(&rc->pids_lock);
	for (ofs = 0; ofs < MAX_ALIVE_THREADS; ofs++) {
		__check_ofs(ofs);
		if (!rc->pids[ofs]) {
			PIDPRINTK(" end!\n");
			//spin_unlock(&rc->pids_lock);
			return;
		} else
			PIDPRINTK("[%d] ", rc->pids[ofs]);
	}
	//spin_unlock(&rc->pids_lock);

	PIDPRINTK("[NULL]\n");
	return;
}

/* should also do at fork... if(rc)*/
void __recreate_pids_list(struct remote_context *rc)
{
	struct task_struct *g = current, *p = current;
	int ofs = 1; /* current */

	spin_lock(&rc->pids_lock);
	rc->pids[0] = current->pid;

	read_lock(&tasklist_lock);
	while_each_thread(g, p) {
		__check_ofs(ofs);
		rc->pids[ofs] = p->pid;
		ofs++;
		//printk("%d ", p->pid);
	}
	read_unlock(&tasklist_lock);

	__print_pids(rc);
	spin_unlock(&rc->pids_lock);
	PIDPRINTK("First thread cnt %d\n", ofs);
}

/* only check me */
void __dynamic_updatepids_list(void)
{
	int ofs = 1; /* current */
	int found = 0;
	int first_empty = 0;
	struct remote_context *rc = get_task_remote(current);

	spin_lock(&rc->pids_lock);
	for (ofs = 0; ofs < MAX_ALIVE_THREADS; ofs++) {
		__check_ofs(ofs);
		if (rc->pids[ofs] == current->pid) {
			BUG_ON(found && "double found!!");
			found = 1;
			PIDPRINTK("[%d] found at [%d]\n", current->pid, ofs);
		}
		if (!rc->pids[ofs]) {
			first_empty = ofs;
			break;
		}
	}
	if (ofs >= MAX_ALIVE_THREADS -1)
		BUG_ON("cannot append - full array");

	if (!found) {
		rc->pids[first_empty] = current->pid;
		PIDPRINTK("dynamically append %d at [%d]\n", current->pid, first_empty);
	}

	__print_pids(rc);
	spin_unlock(&rc->pids_lock);

	__put_task_remote(rc);
}

void __add_pid(struct remote_context *rc, int pid)
{
	int slot;
	//if (slot < 0) return -1;

	//spin_lock(&rc->pids_lock);
	slot = __first_empty_pid_slot(rc);
	rc->pids[slot] = pid;
	//spin_unlock(&rc->pids_lock);

	/* dbg */
	//if (slot == 7)
		__print_pids(rc); // O
}

void __del_pid(struct remote_context *rc, int pid)
{
	int tail, head;

	//spin_lock(&rc->pids_lock);
	tail = __first_empty_pid_slot(rc);
	head = __pid_slot(rc, pid);

	rc->pids[head] = 0;

	/* sort it */
	while (head + 1 < MAX_ALIVE_THREADS) {
		if (head + 1 != tail) {
			rc->pids[head] = rc->pids[head + 1];
			rc->pids[head + 1] = 0;
		}
		head++;
	}
	//spin_unlock(&rc->pids_lock);
}

static void	__out_thread(void)
{
	struct remote_context *rc = get_task_remote(current);

	spin_lock(&rc->pids_lock);
	rc->out_threads++;
	__del_pid(rc, current->pid);

	spin_unlock(&rc->pids_lock);
	PSPRINTK("\t\t--[%d]\n", current->pid);
	__put_task_remote(rc);
}

static void	__back_thread(void)
{
	struct remote_context *rc = get_task_remote(current);
	spin_lock(&rc->pids_lock);
	rc->out_threads--;
	__add_pid(rc, current->pid);
	spin_unlock(&rc->pids_lock);
	__put_task_remote(rc);
}

static int __get_threads_cnt(void)
{
	struct task_struct *g = current, *p = current;
	int cnt = 1; /* current */

	read_lock(&tasklist_lock);
	while_each_thread(g, p) {
		cnt++;
		//PIDPRINTK("%d ", p->pid);
	}
	read_unlock(&tasklist_lock);

	return cnt;
}

static int __get_out_threads_cnt(void)
{
	struct remote_context *rc = get_task_remote(current);
	int cnt;

	spin_lock(&rc->pids_lock);
	cnt = rc->out_threads;
	spin_unlock(&rc->pids_lock);

	__put_task_remote(rc);
	return cnt;
}

static void __recalc_thread_cnt(void)
{
	struct remote_context *rc = get_task_remote(current);

	//PIDPRINTK("rc->for_remote (%s)\n", rc->for_remote?"O":"X");
	rc->threads_cnt = __get_threads_cnt() - __get_out_threads_cnt();
	atomic_set(&rc->barrier, rc->threads_cnt);
	atomic_set(&rc->barrier_end, rc->threads_cnt);
	PSPRINTK("\t\t[%d] alive/total %d/%d\n",
				current->pid, rc->threads_cnt, __get_threads_cnt());

	__put_task_remote(rc);

	/* dgb */
	spin_lock(&rc->pids_lock);
	if (rc->threads_cnt == 8)
		__print_pids(rc);
	spin_unlock(&rc->pids_lock);
}

#if GLOBAL
#if !HASH_GLOBAL
/* perfermance sucks */
static void __sort_array(struct remote_context *rc, int ofs)
{
	int i = ofs;
	while (i + 1 < rc->inv_cnt ) {
		/* lock? in serial phase protected by rc->ready */
		rc->inv_addrs[i] = rc->inv_addrs[i + 1];
		rc->inv_addrs[i + 1] = 0; /* (i+1)th = 0 */
		memcpy(&rc->inv_pages[i * PAGE_SIZE],
				&rc->inv_pages[(i + 1) * PAGE_SIZE], PAGE_SIZE);
		memset(&rc->inv_pages[(i + 1) * PAGE_SIZE], 0, PAGE_SIZE);
		i++;
	}
}
#endif

/* ugly */
extern void sync_clear_page_owner(int nid, struct mm_struct *mm, unsigned long addr);
//void maintain_origin_table(unsigned long target_addr, int i, int total)
void maintain_origin_table(unsigned long target_addr)
{
	/* TODO try to spin_lock(ptl); spin_unlock(ptl); this operations */



//owner is oalway origin in my fucking case
//owner is oalway origin in my fucking case
//owner is oalway origin in my fucking case
//owner is oalway origin in my fucking case
//owner is oalway origin in my fucking case
//owner is oalway origin in my fucking case
//owner is oalway origin in my fucking case
//owner is oalway origin in my fucking case
//owner is oalway origin in my fucking case
//owner is oalway origin in my fucking case
//owner is oalway origin in my fucking case


// but I should now do it here I think......... i should do as normal here and later on do it again in the barrier begin

	if (my_nid == 0)
		sync_clear_page_owner(1, current->mm, target_addr);
	else
		sync_clear_page_owner(0, current->mm, target_addr);
#if 0
//	int peers;
	unsigned long offset, *pi;
	struct page *pip = __get_page_info_page(current->mm, target_addr, &offset);
	if (!pip) BUG(); /* non-distributed page */
	pi = (unsigned long *)kmap(pip) + offset;
#if 0
	peers = bitmap_weight(pi, MAX_POPCORN_NODES);
	if (!peers) { /* skip the page that is not distributed */
		SYNCPRINTK2("%s: [%d] NOT PERFECT WORKAROUND [%d/%d] 0x%lx "
			"skip at here. HOPEFULLY another node will also skip it "
			"(Ithinkso)\n", __func__,
			current->pid, i, total, target_addr);
		goto out;
	}
#endif /* currently enforce to maintain the bit */
	if (my_nid == 0)
		clear_bit(1, pi); /* hardcode */
	else
		clear_bit(0, pi); /* hardcode */
//out:
	kunmap(pip);
#endif
}


#if !HASH_GLOBAL
/*	lock free - serial region
 *  global array case - optimize O(N^2) + sorting array(bad mem copy pages)
 */
int sync_server_local_serial_conflictions(struct remote_context *rc)
{
	int i, j, new_local_wr_cnt = 0, dealed = 0; /* diffs/final cnt */
	for (i = 0; i < rc->inv_cnt; i++) {
		unsigned long addr = rc->inv_addrs[i];
		if(!addr) continue; /* may be removed already */
		for (j = i; j < rc->inv_cnt; j++) {
			unsigned long target_addr = rc->inv_addrs[j];
			if (i == j) continue;
			if (addr == target_addr) { /* local conflict - remove late one */
				rc->inv_addrs[j] = 0;
				memset(&rc->inv_pages[j * PAGE_SIZE], 0, PAGE_SIZE);
				new_local_wr_cnt++; /* local_conflict */
			}
		}
		/////////////////////////////////////////////////////////////////
		/////////////////////////////////////////////////////////////////
		/////////////////////////////////////////////////////////////////
		maintain_origin_table(addr,i, rc->inv_cnt); /* remove or change */
		/////////////////////////////////////////////////////////////////
		/////////////////////////////////////////////////////////////////
		/////////////////////////////////////////////////////////////////
	}

	/* dbg */
	BARRMPRINTK("%d local buffered addr checked\n", rc->inv_cnt);
	if (new_local_wr_cnt)
		BARRMPRINTK("\tlocal_conflict_addr_cnt %d/%d\n",
							new_local_wr_cnt, rc->inv_cnt);

#ifdef CONFIG_POPCORN_CHECK_SANITY
	BUG_ON(new_local_wr_cnt > MAX_ALIVE_THREADS * MAX_WRITE_INV_BUFFERS);
#endif

	/* sorting array */
	for (i = 0; i < rc->inv_cnt; i++) {
		if(!rc->inv_addrs[i]) {
			__sort_array(rc, i); /* perfermance sucks */
			dealed++;
			if (dealed >= new_local_wr_cnt)
				break;
			i = 0;	/* performance sucks */
		}
	}
	new_local_wr_cnt = rc->inv_cnt - new_local_wr_cnt; /* new local_wr_cnt */
	rc->inv_cnt = new_local_wr_cnt; /* update the rc->list for future usage */
	return new_local_wr_cnt;
}
#endif

#else /* !GLOBAL */
void __find_conflict_two_threads(unsigned long addr, struct task_struct *target_tsk, struct remote_context *rc, bool *is_saved)
{
	int j;
	for (j = 0; j < target_tsk->tso_wr_cnt; j++) {
		unsigned long target_addr = target_tsk->buffer_inv_addrs[j];
		if(!target_addr) continue;

		//printk("addr %lx =? target_addr %lx\n", addr, target_addr);
		if (addr == target_addr) { /* local conflict */
			/* remove duplicat */
			target_tsk->buffer_inv_addrs[j] = 0;
			/* save i if not saved, and keep looping all j to */
			if (!*is_saved) {
				*is_saved = true;
				rc->inv_addrs[rc->lconf_cnt] = addr;
				rc->lconf_cnt++;
			}
		}
	}
	//printk("\t\t %llu\n", target_tsk->tso_wr_cnt);
}

int sync_server_local_conflictions(struct remote_context *rc)
{
	int i, j;
	int local_wr_cnt = 0;

    spin_lock(&rc->pids_lock);
	// O(n^2): all threads compare with all threads
    for (i = 0; i < MAX_ALIVE_THREADS; i++) {
		int pid, ii;
		struct task_struct *tsk;

        __check_ofs(i);
		pid = rc->pids[i];
        if (!pid)
			goto all_done;

		tsk = __get_task_struct(pid);
		if (!tsk) {
			spin_lock(&rc->pids_lock);
			__print_pids(rc);
			spin_unlock(&rc->pids_lock);
			BUG();
		}

#if !GLOBAL
		local_wr_cnt += tsk->tso_wr_cnt;
#else

#endif

		/* addrs_addrs: O(n^2): all addrs compare with all addrs */
		for (ii = 0; ii < tsk->tso_wr_cnt; ii++) {
			unsigned long addr = tsk->buffer_inv_addrs[ii];
			bool is_saved = false;
			if (!addr)
				break; /* end of addr */

//			printk("%s(): check %lx\n", __func__, addr);

			/* compaer a addr with all addrs at all threads */
			for (j = 0; j < MAX_ALIVE_THREADS; j++) {
				int target_pid;
				struct task_struct *target_tsk;
				if (i == j) continue; // TODO: do a sanity check for this, if this happens, we still has to cheat it as a hashset.

				__check_ofs(j);
				target_pid = rc->pids[j];
				if (!target_pid)
					break;

				target_tsk = __get_task_struct(target_pid);
				if (!target_tsk) {
					printk("[%d]i %d [%d]j %d\n", pid, i, target_pid, j);
					BUG();
				}

				/* for addrs in target_thread */
				__find_conflict_two_threads(addr, target_tsk, rc, &is_saved);
			}
			/* Warnning pids[] is 0/full */
		}
		//printk("\t %llu\n", tsk->tso_wr_cnt);
    }
	/* Warnning pids[] is 0/full */
all_done:
//#if LOCAL_CONFLICT_DBG
	if (local_wr_cnt > 0)
		printk("local_wr_cnt %d\n", local_wr_cnt);
//#endif
    spin_unlock(&rc->pids_lock);
	return local_wr_cnt;
}
#endif

///////////////////////////////////////////////////////////////////////////////
// Distributed mutex
///////////////////////////////////////////////////////////////////////////////
long process_server_do_futex_at_remote(u32 __user *uaddr, int op, u32 val,
		bool valid_ts, struct timespec *ts,
		u32 __user *uaddr2,u32 val2, u32 val3)
{
	struct wait_station *ws = get_wait_station(current);
	remote_futex_request req = {
		.origin_pid = current->origin_pid,
		.remote_ws = ws->id,
		.op = op,
		.val = val,
		.ts = {
			.tv_sec = -1,
		},
		.uaddr = uaddr,
		.uaddr2 = uaddr2,
		.val2 = val2,
		.val3 = val3,
	};
	remote_futex_response *res;
	long ret;

	if (valid_ts) {
		req.ts = *ts;
	}


#if FUTEX_DBG
	printk(" f[%d] ->[%d/%d] 0x%x %p 0x%x\n", current->pid,
			current->origin_pid, current->origin_nid,
			op, uaddr, val);
#endif

	pcn_kmsg_send(PCN_KMSG_TYPE_FUTEX_REQUEST,
			current->origin_nid, &req, sizeof(req));
	res = wait_at_station(ws);
	ret = res->ret;
#if FUTEX_DBG
	printk(" f[%d] <-[%d/%d] 0x%x %p %ld\n", current->pid,
			current->origin_pid, current->origin_nid,
			op, uaddr, ret);
#endif

	pcn_kmsg_done(res);
	return ret;
}

static int handle_remote_futex_response(struct pcn_kmsg_message *msg)
{
	remote_futex_response *res = (remote_futex_response *)msg;
	struct wait_station *ws = wait_station(res->remote_ws);

	ws->private = res;
	complete(&ws->pendings);
	return 0;
}

static void process_remote_futex_request(remote_futex_request *req)
{
	int ret;
	remote_futex_response *res;
	ktime_t t, *tp = NULL;

	if (timespec_valid(&req->ts)) {
		t = timespec_to_ktime(req->ts);
		t = ktime_add_safe(ktime_get(), t);
		tp = &t;
	}


#if FUTEX_DBG
	printk(" f[%d] <-[%d/%d] 0x%x %p 0x%x\n", current->pid,
			current->remote_pid, current->remote_nid,
			req->op, req->uaddr, req->val);
#endif

	ret = do_futex(req->uaddr, req->op, req->val,
			tp, req->uaddr2, req->val2, req->val3);

	res = pcn_kmsg_get(sizeof(*res));
	res->remote_ws = req->remote_ws;
	res->ret = ret;

#if FUTEX_DBG
	printk(" f[%d] ->[%d/%d] 0x%x %p %ld\n", current->pid,
			current->remote_pid, current->remote_nid,
			req->op, req->uaddr, res->ret);
#endif

	pcn_kmsg_post(PCN_KMSG_TYPE_FUTEX_RESPONSE,
			current->remote_nid, res, sizeof(*res));
	pcn_kmsg_done(req);
}


///////////////////////////////////////////////////////////////////////////////
// Handle process/task exit
///////////////////////////////////////////////////////////////////////////////
static void __terminate_remotes(struct remote_context *rc)
{
	int nid;
	origin_task_exit_t req = {
		.origin_pid = current->pid,
		.exit_code = current->exit_code,
	};

	/* Take down peer vma workers */
	for (nid = 0; nid < MAX_POPCORN_NODES; nid++) {
		if (nid == my_nid || rc->remote_tgids[nid] == 0) continue;
		PSPRINTK("TERMINATE [%d/%d] with 0x%d\n",
				rc->remote_tgids[nid], nid, req.exit_code);

		req.remote_pid = rc->remote_tgids[nid];
		pcn_kmsg_send(PCN_KMSG_TYPE_TASK_EXIT_ORIGIN, nid, &req, sizeof(req));
	}
}

static int __exit_origin_task(struct task_struct *tsk)
{
	struct remote_context *rc = tsk->mm->remote;

	if (tsk->remote) {
		put_task_remote(tsk);
	}
	tsk->remote = NULL;
	tsk->origin_nid = tsk->origin_pid = -1;

	/**
	 * Trigger peer termination if this is the last user thread
	 * referring to this mm.
	 */
	if (atomic_read(&tsk->mm->mm_users) == 1) {
		__terminate_remotes(rc);
	}

	return 0;
}

static int __exit_remote_task(struct task_struct *tsk)
{
	if (tsk->exit_code == TASK_PARKED) {
		/* Skip notifying for back-migrated threads */
	} else {
		/* Something went south. Notify the origin. */
		if (!get_task_remote(tsk)->stop_remote_worker) {
			remote_task_exit_t req = {
				.origin_pid = tsk->origin_pid,
				.remote_pid = tsk->pid,
				.exit_code = tsk->exit_code,
			};
			pcn_kmsg_send(PCN_KMSG_TYPE_TASK_EXIT_REMOTE,
					tsk->origin_nid, &req, sizeof(req));
		}
		put_task_remote(tsk);
	}

	put_task_remote(tsk);
	tsk->remote = NULL;
	tsk->origin_nid = tsk->origin_pid = -1;

	return 0;
}

extern void collect_tso_wr(struct task_struct *tsk);
extern unsigned long long plock_system_tso_wr_cnt;
int process_server_task_exit(struct task_struct *tsk)
{
	WARN_ON(tsk != current);

	if (!distributed_process(tsk)) return -ESRCH;

	PSPRINTK("EXITED [%d] %s%s / 0x%x BACK_MIGRATE(%s)\n", tsk->pid,
			tsk->at_remote ? "remote" : "local",
			tsk->is_worker ? " worker": "",
			tsk->exit_code, tsk->exit_code == TASK_PARKED ? "O" : "X");

	// show_regs(task_pt_regs(tsk));

	if (tsk->is_worker) return 0;

	collect_tso_wr(tsk);

	if (tsk->at_remote) {
		return __exit_remote_task(tsk);
	} else {
		return __exit_origin_task(tsk);
	}
}


/**
 * Handle the notification of the task kill at the remote.
 */
static void process_remote_task_exit(remote_task_exit_t *req)
{
	struct task_struct *tsk = current;
	int exit_code = req->exit_code;

	if (tsk->remote_pid != req->remote_pid) {
		printk(KERN_INFO"%s: pid mismatch %d != %d\n", __func__,
				tsk->remote_pid, req->remote_pid);
		pcn_kmsg_done(req);
		return;
	}

	PSPRINTK("%s [%d] 0x%x\n", __func__, tsk->pid, req->exit_code);

	tsk->remote = NULL;
	tsk->remote_nid = -1;
	tsk->remote_pid = -1;
	put_task_remote(tsk);

	exit_code = req->exit_code;
	pcn_kmsg_done(req);

	if (exit_code & CSIGNAL) {
		force_sig(exit_code & CSIGNAL, tsk);
	}
	do_exit(exit_code);
}

static void process_origin_task_exit(struct remote_context *rc, origin_task_exit_t *req)
{
	BUG_ON(!current->is_worker);

	PSPRINTK("\nTERMINATE [%d] with 0x%x\n", current->pid, req->exit_code);
	current->exit_code = req->exit_code;
	rc->stop_remote_worker = true;

	pcn_kmsg_done(req);
}


///////////////////////////////////////////////////////////////////////////////
// handling back migration
///////////////////////////////////////////////////////////////////////////////
static void process_back_migration(back_migration_request_t *req)
{
	if (current->remote_pid != req->remote_pid) {
		printk(KERN_INFO"%s: pid mismatch during back migration (%d != %d)\n",
				__func__, current->remote_pid, req->remote_pid);
		goto out_free;
	}

	PSPRINTK("### BACKMIG [%d] from [%d/%d]\n",
			current->pid, req->remote_pid, req->remote_nid);

	/* Welcome home */

	__back_thread();
	__recalc_thread_cnt();

	current->remote = NULL;
	current->remote_nid = -1;
	current->remote_pid = -1;
	put_task_remote(current);

	current->personality = req->personality;

	/* XXX signals */

	/* mm is not updated here; has been synchronized through vma operations */

	restore_thread_info(&req->arch, true);

out_free:
	pcn_kmsg_done(req);
}


/*
 * Send a message to <dst_nid> for migrating back a task <task>.
 * This is a back migration
 *  => <task> must already been migrated to <dst_nid>.
 * It returns -1 in error case.
 */
static int __do_back_migration(struct task_struct *tsk, int dst_nid, void __user *uregs)
{
	back_migration_request_t *req;
	int ret;
	struct remote_context *rc = get_task_remote(current);

	might_sleep();

	BUG_ON(tsk->origin_nid == -1 && tsk->origin_pid == -1);

	req = pcn_kmsg_get(sizeof(*req));

	req->origin_pid = tsk->origin_pid;
	req->remote_nid = my_nid;
	req->remote_pid = tsk->pid;

	req->personality = tsk->personality;

	/*
	req->remote_blocked = tsk->blocked;
	req->remote_real_blocked = tsk->real_blocked;
	req->remote_saved_sigmask = tsk->saved_sigmask;
	req->remote_pending = tsk->pending;
	req->sas_ss_sp = tsk->sas_ss_sp;
	req->sas_ss_size = tsk->sas_ss_size;
	memcpy(req->action, tsk->sighand->action, sizeof(req->action));
	*/

	/* TODO: see if can be functionlized */
	spin_lock(&rc->pids_lock);
	rc->threads_cnt--;
	__del_pid(rc, current->pid);
	spin_unlock(&rc->pids_lock);
	atomic_set(&rc->barrier, rc->threads_cnt);
	atomic_set(&rc->pendings, 0);
	atomic_set(&rc->scatter_pendings, 0);
	atomic_set(&rc->barrier_end, rc->threads_cnt);
	PSPRINTK("\t\t--[%d] alive %hu\n", current->pid, rc->threads_cnt);
	__put_task_remote(rc);

	ret = copy_from_user(&req->arch.regsets, uregs,
			regset_size(get_popcorn_node_arch(dst_nid)));
	BUG_ON(ret != 0);

	save_thread_info(&req->arch);

	ret = pcn_kmsg_post(
			PCN_KMSG_TYPE_TASK_MIGRATE_BACK, dst_nid, req, sizeof(*req));

	do_exit(TASK_PARKED);
}


///////////////////////////////////////////////////////////////////////////////
// Remote thread
///////////////////////////////////////////////////////////////////////////////
static int handle_remote_task_pairing(struct pcn_kmsg_message *msg)
{	/* at origin */
	remote_task_pairing_t *req = (remote_task_pairing_t *)msg;
	struct task_struct *tsk;
	int from_nid = PCN_KMSG_FROM_NID(req);
	int ret = 0;

	tsk = __get_task_struct(req->your_pid);
	if (!tsk) {
		ret = -ESRCH;
		goto out;
	}
	BUG_ON(tsk->at_remote);
	BUG_ON(!tsk->remote);

	tsk->remote_nid = from_nid;
	tsk->remote_pid = req->my_pid;
	tsk->remote->remote_tgids[from_nid] = req->my_tgid;

	put_task_struct(tsk);
out:
	pcn_kmsg_done(req);
	return 0;
}

static int __pair_remote_task(void)
{
	remote_task_pairing_t req = {
		.my_tgid = current->tgid,
		.my_pid = current->pid,
		.your_pid = current->origin_pid,
	};
	return pcn_kmsg_send(
			PCN_KMSG_TYPE_TASK_PAIRING, current->origin_nid, &req, sizeof(req));
}


struct remote_thread_params {
	clone_request_t *req;
};

/* add a thread at remote (not remote_main_worker) */
static int remote_thread_main(void *_args)
{
	struct remote_thread_params *params = _args;
	clone_request_t *req = params->req;
	struct remote_context *rc = get_task_remote(current);

	/* TODO: see if can be functionlized */
	// TODO: make two functions ++/--
	//PSPRINTK("rc->for_remote (%s)\n", rc->for_remote?"O":"X");
	spin_lock(&rc->pids_lock);
	rc->threads_cnt++; /* TODO: check if serialized by sponer */
	__add_pid(rc, current->pid);
	atomic_set(&rc->barrier, rc->threads_cnt);
	atomic_set(&rc->barrier_end, rc->threads_cnt);
	spin_unlock(&rc->pids_lock);
	PSPRINTK("\t\t++[%d] alive %hu\n", current->pid, rc->threads_cnt); /* TODO: check if serialized by sponer */
	__put_task_remote(rc);

#ifdef CONFIG_POPCORN_DEBUG_VERBOSE
	PSPRINTK("%s [%d] started for [%d/%d]\n", __func__,
			current->pid, req->origin_pid, PCN_KMSG_FROM_NID(req));
#endif

	current->flags &= ~PF_KTHREAD;	/* Demote from temporary priviledge */
	current->origin_nid = PCN_KMSG_FROM_NID(req);
	current->origin_pid = req->origin_pid;
	current->remote = get_task_remote(current);

	set_fs(USER_DS);

	/* Inject thread info here */
	restore_thread_info(&req->arch, true);

	/* XXX: Skip restoring signals and handlers for now
	sigorsets(&current->blocked, &current->blocked, &req->remote_blocked);
	sigorsets(&current->real_blocked,
			&current->real_blocked, &req->remote_real_blocked);
	sigorsets(&current->saved_sigmask,
			&current->saved_sigmask, &req->remote_saved_sigmask);
	current->pending = req->remote_pending;
	current->sas_ss_sp = req->sas_ss_sp;
	current->sas_ss_size = req->sas_ss_size;
	memcpy(current->sighand->action, req->action, sizeof(req->action));
	*/

	__pair_remote_task();

	PSPRINTK("\n####### MIGRATED - [%d/%d] from [%d/%d]\n",
			current->pid, my_nid, current->origin_pid, current->origin_nid);

	kfree(params);
	pcn_kmsg_done(req);

	return 0;
	/* Returning from here makes this thread jump into the user-space */
}

static int __fork_remote_thread(clone_request_t *req)
{
	struct remote_thread_params *params;
	params = kmalloc(sizeof(*params), GFP_KERNEL);
	params->req = req;

	/* The loop deals with signals between concurrent migration */
	while (kernel_thread(remote_thread_main, params,
					CLONE_THREAD | CLONE_SIGHAND | SIGCHLD) < 0) {
		schedule();
	}
	return 0;
}

static int __construct_mm(clone_request_t *req, struct remote_context *rc)
{
	struct mm_struct *mm;
	struct file *f;

	mm = mm_alloc();
	if (!mm) {
		return -ENOMEM;
	}

	arch_pick_mmap_layout(mm);

	f = filp_open(req->exe_path, O_RDONLY | O_LARGEFILE | O_EXCL, 0);
	if (IS_ERR(f)) {
		PCNPRINTK_ERR("cannot open executable from %s\n", req->exe_path);
		mmdrop(mm);
		return -EINVAL;
	}
	set_mm_exe_file(mm, f);
	filp_close(f, NULL);

	mm->task_size = req->task_size;
	mm->start_stack = req->stack_start;
	mm->start_brk = req->start_brk;
	mm->brk = req->brk;
	mm->env_start = req->env_start;
	mm->env_end = req->env_end;
	mm->arg_start = req->arg_start;
	mm->arg_end = req->arg_end;
	mm->start_code = req->start_code;
	mm->end_code = req->end_code;
	mm->start_data = req->start_data;
	mm->end_data = req->end_data;
	mm->def_flags = req->def_flags;

	use_mm(mm);

	rc->mm = mm;  /* No need to increase mm_users due to mm_alloc() */
	mm->remote = rc;

	return 0;
}


static void __terminate_remote_threads(struct remote_context *rc)
{
	struct task_struct *tsk;

	/* Terminate userspace threads. Tried to use do_group_exit() but it
	 * didn't work */
	rcu_read_lock();
	for_each_thread(current, tsk) {
		if (tsk->is_worker) continue;
		force_sig(current->exit_code, tsk);
	}
	rcu_read_unlock();
}

static void __run_remote_worker(struct remote_context *rc)
{
	while (!rc->stop_remote_worker) {
		struct work_struct *work = NULL;
		struct pcn_kmsg_message *msg;
		int ret;
		unsigned long flags;

		ret = wait_for_completion_interruptible_timeout(
					&rc->remote_works_ready, HZ);
		if (ret == 0) continue;

		spin_lock_irqsave(&rc->remote_works_lock, flags);
		if (!list_empty(&rc->remote_works)) {
			work = list_first_entry(
					&rc->remote_works, struct work_struct, entry);
			list_del(&work->entry);
		}
		spin_unlock_irqrestore(&rc->remote_works_lock, flags);
		if (!work) continue;

		msg = ((struct pcn_kmsg_work *)work)->msg;

		switch (msg->header.type) {
		case PCN_KMSG_TYPE_TASK_MIGRATE:
			__fork_remote_thread((clone_request_t *)msg);
			break;
		case PCN_KMSG_TYPE_VMA_OP_REQUEST:
			process_vma_op_request((vma_op_request_t *)msg);
			break;
		case PCN_KMSG_TYPE_TASK_EXIT_ORIGIN:
			process_origin_task_exit(rc, (origin_task_exit_t *)msg);
			break;
		default:
			printk("Unknown remote work type %d\n", msg->header.type);
			break;
		}

		/* msg is released (pcn_kmsg_done()) in each handler */
		kfree(work);
	}
}


struct remote_worker_params {
	clone_request_t *req;
	struct remote_context *rc;
	char comm[TASK_COMM_LEN];
};

static int remote_worker_main(void *data)
{
	struct remote_worker_params *params = (struct remote_worker_params *)data;
	struct remote_context *rc = params->rc;
	clone_request_t *req = params->req;

	might_sleep();
	kfree(params);

	PSPRINTK("%s: r[%d] for o[%d/%d]\n", __func__,
			current->pid, req->origin_tgid, PCN_KMSG_FROM_NID(req));
	PSPRINTK("%s: r[%d] %s\n", __func__,
			current->pid, req->exe_path);

	current->flags &= ~PF_RANDOMIZE;	/* Disable ASLR for now*/
	current->flags &= ~PF_KTHREAD;	/* Demote to a user thread */

	current->personality = req->personality;
	current->is_worker = true;
	current->at_remote = true;
	current->origin_nid = PCN_KMSG_FROM_NID(req);
	current->origin_pid = req->origin_pid;

	set_user_nice(current, 0);

	/* meaningless for now */
	/*
	struct cred *new;
	new = prepare_kernel_cred(NULL);
	commit_creds(new);
	*/

	if (__construct_mm(req, rc)) {
		BUG();
		return -EINVAL;
	}

	get_task_remote(current);
	rc->tgid = current->tgid;

	__run_remote_worker(rc);

	__terminate_remote_threads(rc);

	put_task_remote(current);
	return current->exit_code;
}



static void __schedule_remote_work(struct remote_context *rc, struct pcn_kmsg_work *work)
{
	/* Exploit the list_head in work_struct */
	struct list_head *entry = &((struct work_struct *)work)->entry;
	unsigned long flags;

	INIT_LIST_HEAD(entry);
	spin_lock_irqsave(&rc->remote_works_lock, flags);
	list_add(entry, &rc->remote_works);
	spin_unlock_irqrestore(&rc->remote_works_lock, flags);

	complete(&rc->remote_works_ready);
}

static void clone_remote_thread(struct work_struct *_work)
{
	struct pcn_kmsg_work *work = (struct pcn_kmsg_work *)_work;
	clone_request_t *req = work->msg;
	int nid_from = PCN_KMSG_FROM_NID(req);
	int tgid_from = req->origin_tgid;
	struct remote_context *rc;
	struct remote_context *rc_new =
			__alloc_remote_context(nid_from, tgid_from, true);

	BUG_ON(!rc_new);

	__lock_remote_contexts_in(nid_from);
	rc = __lookup_remote_contexts_in(nid_from, tgid_from);
	if (!rc) {
		struct remote_worker_params *params;

		rc = rc_new;
		rc->remote_tgids[nid_from] = tgid_from;
		list_add(&rc->list, &__remote_contexts_in());
		__unlock_remote_contexts_in(nid_from);

		params = kmalloc(sizeof(*params), GFP_KERNEL);
		BUG_ON(!params);

		params->rc = rc;
		params->req = req;
		__build_task_comm(params->comm, req->exe_path);
		smp_wmb();

		rc->remote_worker =
				kthread_run(remote_worker_main, params, params->comm);
		__sync_init(rc);
	} else {
		__unlock_remote_contexts_in(nid_from);
		kfree(rc_new);
	}

	/* Schedule this fork request */
	__schedule_remote_work(rc, work);
	return;
}

static int handle_clone_request(struct pcn_kmsg_message *msg)
{
	clone_request_t *req = (clone_request_t *)msg;
	struct pcn_kmsg_work *work = kmalloc(sizeof(*work), GFP_ATOMIC);
	BUG_ON(!work);

	work->msg = req;
	INIT_WORK((struct work_struct *)work, clone_remote_thread);
	queue_work(popcorn_wq, (struct work_struct *)work);

	return 0;
}


///////////////////////////////////////////////////////////////////////////////
// Handle remote works at the origin
///////////////////////////////////////////////////////////////////////////////
int request_remote_work(pid_t pid, struct pcn_kmsg_message *req)
{
	struct task_struct *tsk = __get_task_struct(pid);
	int ret = -ESRCH;
	if (!tsk) {
		int i = 0;
		printk(KERN_INFO"%s: invalid origin task %d for remote work %d\n",
				__func__, pid, req->header.type);
		WARN_ON("trying to fix");
		while (!tsk) {
			if (++i > 100) BUG();
			tsk = __get_task_struct(pid);
			io_schedule();
		}
		printk(KERN_INFO"%s: fixed origin task %d for remote work %d\n",
										__func__, pid, req->header.type);
		//goto out_err;
	}

	/**
	 * Origin-initiated remote works are node-wide operations, thus, enqueue
	 * such requests into the remote work queue.
	 * On the other hand, remote-initated remote works are thread-wise requests.
	 * So, pending the requests to the per-thread work queue.
	 */
	if (tsk->at_remote) {
		struct remote_context *rc = get_task_remote(tsk);
		struct pcn_kmsg_work *work = kmalloc(sizeof(*work), GFP_ATOMIC);

		BUG_ON(!tsk->is_worker);
		work->msg = req;

		__schedule_remote_work(rc, work);

		__put_task_remote(rc);
	} else {
		WARN_ON(tsk->remote_work);
		while (tsk->remote_work) // Jack DEX BUG FIX
			; // Jack DEX BUG FIX
		tsk->remote_work = req;
		complete(&tsk->remote_work_pended); /* implicit memory barrier */
	}

	put_task_struct(tsk);
	return 0;

//out_err:
	pcn_kmsg_done(req);
	return ret;
}

static void __process_remote_works(void)
{
	bool run = true;
	BUG_ON(current->at_remote);

	while (run) {
		struct pcn_kmsg_message *req;
		long ret;
		ret = wait_for_completion_interruptible_timeout(
				&current->remote_work_pended, HZ);
		if (ret == 0) continue; /* timeout */

		req = (struct pcn_kmsg_message *)current->remote_work;
		current->remote_work = NULL;
		smp_wmb();

		if (!req) continue;

		switch (req->header.type) {
		case PCN_KMSG_TYPE_REMOTE_PAGE_REQUEST:
			WARN_ON_ONCE("Not implemented yet!");
			break;
		case PCN_KMSG_TYPE_VMA_OP_REQUEST:
			process_vma_op_request((vma_op_request_t *)req);
			break;
		case PCN_KMSG_TYPE_VMA_INFO_REQUEST:
			process_vma_info_request((vma_info_request_t *)req);
			break;
		case PCN_KMSG_TYPE_FUTEX_REQUEST:
			process_remote_futex_request((remote_futex_request *)req);
			break;
		case PCN_KMSG_TYPE_TASK_EXIT_REMOTE:
			process_remote_task_exit((remote_task_exit_t *)req);
			run = false;
			break;
		case PCN_KMSG_TYPE_TASK_MIGRATE_BACK:
			process_back_migration((back_migration_request_t *)req);
			run = false;
			break;
		default:
			if (WARN_ON("Received unsupported remote work")) {
				printk("  type: %d\n", req->header.type);
			}
		}
	}
}


/**
 * Send a message to <dst_nid> for migrating a task <task>.
 * This function will ask the remote node to create a thread to host the task.
 * It returns <0 in error case.
 */
static int __request_clone_remote(int dst_nid, struct task_struct *tsk, void __user *uregs)
{
	struct mm_struct *mm = get_task_mm(tsk);
	clone_request_t *req;
	int ret;

	req = pcn_kmsg_get(sizeof(*req));
	if (!req) {
		ret = -ENOMEM;
		goto out;
	}

	/* struct mm_struct */
	if (get_file_path(mm->exe_file, req->exe_path, sizeof(req->exe_path))) {
		printk("%s: cannot get path to exe binary\n", __func__);
		ret = -ESRCH;
		pcn_kmsg_put(req);
		goto out;
	}

	req->task_size = mm->task_size;
	req->stack_start = mm->start_stack;
	req->start_brk = mm->start_brk;
	req->brk = mm->brk;
	req->env_start = mm->env_start;
	req->env_end = mm->env_end;
	req->arg_start = mm->arg_start;
	req->arg_end = mm->arg_end;
	req->start_code = mm->start_code;
	req->end_code = mm->end_code;
	req->start_data = mm->start_data;
	req->end_data = mm->end_data;
	req->def_flags = mm->def_flags;

	/* struct tsk_struct */
	req->origin_tgid = tsk->tgid;
	req->origin_pid = tsk->pid;

	req->personality = tsk->personality;

	/* Signals and handlers
	req->remote_blocked = tsk->blocked;
	req->remote_real_blocked = tsk->real_blocked;
	req->remote_saved_sigmask = tsk->saved_sigmask;
	req->remote_pending = tsk->pending;
	req->sas_ss_sp = tsk->sas_ss_sp;
	req->sas_ss_size = tsk->sas_ss_size;
	memcpy(req->action, tsk->sighand->action, sizeof(req->action));
	*/

	/* Register sets from userspace */
	ret = copy_from_user(&req->arch.regsets, uregs,
			regset_size(get_popcorn_node_arch(dst_nid)));
	BUG_ON(ret != 0);
	save_thread_info(&req->arch);

	ret = pcn_kmsg_post(PCN_KMSG_TYPE_TASK_MIGRATE, dst_nid, req, sizeof(*req));

out:
	mmput(mm);
	return ret;
}

static int __do_migration(struct task_struct *tsk, int dst_nid, void __user *uregs)
{
	int ret;
	struct remote_context *rc;

	/* Won't to allocate this object in a spinlock-ed area */
	rc = __alloc_remote_context(my_nid, tsk->tgid, false);
	if (IS_ERR(rc)) return PTR_ERR(rc);

	if (cmpxchg(&tsk->mm->remote, 0, rc)) {
		kfree(rc);
	} else {
		/*
		 * This process is becoming a distributed one if it was not yet.
		 * The first thread gets migrated attaches the remote context to
		 * mm->remote, which indicates some threads in this process is
		 * distributed.
		 */
		__recreate_pids_list(rc); /* race condiction with __out_thread() below */
		rc->mm = tsk->mm;
		rc->remote_tgids[my_nid] = tsk->tgid;

		__lock_remote_contexts_out(dst_nid);
		list_add(&rc->list, &__remote_contexts_out());
		__unlock_remote_contexts_out(dst_nid);

		__sync_init(rc);
	}
	/*
	 * tsk->remote != NULL implies this thread is distributed (migrated away).
	 */
	tsk->remote = get_task_remote(tsk);

	/* every time recreate, otherwise race condition - cannot find pid to del */
	__dynamic_updatepids_list();
	/* TODO: see if can be functionlized */
	__out_thread();
	__recalc_thread_cnt();

	ret = __request_clone_remote(dst_nid, tsk, uregs);
	if (ret) return ret;

	__process_remote_works(); /* migrate then wait for handling requests */
	return 0;
}


/**
 * Migrate the specified task <task> to node <dst_nid>
 * Currently, this function will put the specified task to sleep,
 * and push its info over to the remote node.
 * The remote node will then create a new thread and import that
 * info into its new context.
 */
int process_server_do_migration(struct task_struct *tsk, unsigned int dst_nid, void __user *uregs)
{
	int ret = 0;

	if (tsk->origin_nid == dst_nid) {
		ret = __do_back_migration(tsk, dst_nid, uregs);
	} else {
		ret = __do_migration(tsk, dst_nid, uregs);
		if (ret) {
			tsk->remote = NULL;
			tsk->remote_pid = tsk->remote_nid = -1;
			put_task_remote(tsk);
		}
	}

	return ret;
}


DEFINE_KMSG_RW_HANDLER(origin_task_exit, origin_task_exit_t, remote_pid);
DEFINE_KMSG_RW_HANDLER(remote_task_exit, remote_task_exit_t, origin_pid);
DEFINE_KMSG_RW_HANDLER(back_migration, back_migration_request_t, origin_pid);
DEFINE_KMSG_RW_HANDLER(remote_futex_request, remote_futex_request, origin_pid);

/**
 * Initialize the process server.
 */
int __init process_server_init(void)
{
	INIT_LIST_HEAD(&remote_contexts[0]);
	INIT_LIST_HEAD(&remote_contexts[1]);

	spin_lock_init(&remote_contexts_lock[0]);
	spin_lock_init(&remote_contexts_lock[1]);

	/* Register handlers */
	REGISTER_KMSG_HANDLER(PCN_KMSG_TYPE_TASK_MIGRATE, clone_request);
	REGISTER_KMSG_HANDLER(PCN_KMSG_TYPE_TASK_MIGRATE_BACK, back_migration);
	REGISTER_KMSG_HANDLER(PCN_KMSG_TYPE_TASK_PAIRING, remote_task_pairing);

	REGISTER_KMSG_HANDLER(PCN_KMSG_TYPE_TASK_EXIT_REMOTE, remote_task_exit);
	REGISTER_KMSG_HANDLER(PCN_KMSG_TYPE_TASK_EXIT_ORIGIN, origin_task_exit);

	REGISTER_KMSG_HANDLER(PCN_KMSG_TYPE_FUTEX_REQUEST, remote_futex_request);
	REGISTER_KMSG_HANDLER(PCN_KMSG_TYPE_FUTEX_RESPONSE, remote_futex_response);

	return 0;
}
