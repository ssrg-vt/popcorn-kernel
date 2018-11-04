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
#include <linux/highmem.h>

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


/************** working zone *************/
#define SMART_REGION_PERF_DBG 1

#define GOD_VIEW 1

/* Tuning knobs */
#define PREFETCH_THRESHOLD 20

#define SMART_REGION_DBG 0 /* to debug smart region working behaviour */
/************** working zone *************/

#define PERF_FULL_BULL_WARN 0 // only for prink statis still works and show in the end !!!
#define IGNORE_WARN_ON_ONCE 1

#define PREFETCH_WRITE 0 /* NOT READY */

#define STATIS 1

#define DECISION_PREFETCH_STATIS 0
#define STRONG_CHECK_SANITY 0
#define SKIP_MEM_CLEAN 0

#define CONSERVATIVE 0 // this is BETA 1:safe

#define CPU_RELAX io_schedule()
//#define CPU_RELAX

#define X86_THREADS 16
#define ARM_THREADS 96
#define MAX_THREADS ARM_THREADS
#define MSG_ROOM_FOR_OTHER_MSG (ARM_THREADS / 2)


#define POPCORN_TSO_BARRIER 1
#define REENTRY_BEGIN_DISABLE 1 // 0: repeat show 1: once

#define SMART_REGION 1 // buggy in local_find_condlict. sync problem when 1

#define PREFETCH_VERBOSE_DBG 0
#if PREFETCH_VERBOSE_DBG
#define PFVPRINTK(...) printk(KERN_INFO __VA_ARGS__)
#else
#define PFVPRINTK(...)
#endif

#define WW_SYNC_VERBOSE_DBG 0
#if WW_SYNC_VERBOSE_DBG
#define WSYPRINTK(...) printk(KERN_INFO __VA_ARGS__)
#else
#define WSYPRINTK(...)
#endif





#define SMART_REGION_DBG_DBG 0
#if SMART_REGION_DBG_DBG
#define SMRPRINTK(...) printk(KERN_INFO __VA_ARGS__)
#else
#define SMRPRINTK(...)
#endif

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

#define RGN_DEBUG_THIS 0
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

/* rcsi meta/mem hash lock per region (sys-wide) */
#define RCSI_HASH_BITS 10 /* TODO try larger */
DEFINE_HASHTABLE(rcsi_hash, RCSI_HASH_BITS);
DEFINE_SPINLOCK(rcsi_hash_lock);

/* GOD VIEW */
#define SYS_REGION_HASH_BITS 10 /* TODO try larger */
DEFINE_HASHTABLE(sys_region_hash, SYS_REGION_HASH_BITS);
DEFINE_RWLOCK(sys_region_hash_lock);


/* Violation section detecting */
static bool print = false;
#if !SMART_REGION
static bool print_end = false;
#endif

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

#define PER_REGION_HASH_BITS 15 /* TODO testing this value */
#define EVERY_REGION_HASH_BITS 15 /* TODO testing this value */
#define BEHAVIOR_SAMPLE_CNT 100
struct omp_region {
	struct hlist_node hentry;
	unsigned long id;		/* region id/hash key */
	int cnt;				/* for really synchronously run in the region */
	/* status */
	int vain_cnt;	/* wr */
	unsigned long type;

	/* statis */
	int in_region_inv_sum; // per region
	int in_region_conflict_sum;

	/* dynamic prefetch - region based
	   Since this page is recorded, it's impossible to be out-of-boundry */
	int read_fault_cnt;
	unsigned long read_max; /* read fault max addr */
	unsigned long read_min; /* read fault min addr */
	bool ht_selector; /* per region hash table selector */
	DECLARE_HASHTABLE(per_region_hash, PER_REGION_HASH_BITS);
	rwlock_t per_region_hash_lock;
	/* Read Fault - lud serial two regions */

	// log 1 to determin (needed? pf should use other interface )
#if SMART_REGION_DBG
	/* dbg */
	/* readability */
	char name[256];
	int line;
	/* statis */
	atomic_t total_cnt; // per thread
	atomic_t skip_cnt; // per thread
#endif
};

/* Prefetch */
struct readfault_info {
	struct hlist_node hentry;
	unsigned long addr;
};

struct fault_info {
	struct hlist_node hentry;
	unsigned long addr;
};

struct sys_omp_region {
	struct hlist_node hentry;
	unsigned long god_id;	/* god id/hash key */
	unsigned long id;		/* region id/hash key */
	int cnt;				/* for really synchronously run in the region */

#if GOD_VIEW
//	int read_fault_cnt;
	unsigned long read_max; /* read fault max addr - not used */
	unsigned long read_min; /* read fault min addr - not used */

//	int notminewrite_fault_cnt;
	unsigned long notminewrite_max; /* notminewrite fault max addr - not used */
	unsigned long notminewrite_min; /* notminewrite fault min addr - not used */

	int skip_r_cnt;	/* runtime skip */
	int skip_w_cnt;

	int skip_r_cnt_msg_limit; /* msg limit skip */
	int skip_w_cnt_msg_limit;

	spinlock_t region_lock;
	spinlock_t r_lock;
	spinlock_t w_lock;
	int slot_id;
//	int read_cnt[MAX_THREADS];								/* hardcoded */
//	int writenopg_cnt[MAX_THREADS];							/* hardcoded */
//	unsigned long read_addrs[MAX_THREADS][MAX_READ_BUFFERS];/* harcorded */
//	unsigned long writenopg_addrs[MAX_THREADS][MAX_WRITE_NOPAGE_BUFFERS]; //same
	int read_cnt;
	int writenopg_cnt;
	unsigned long read_addrs[MAX_READ_BUFFERS * MAX_THREADS / 10]; // harcorded
	unsigned long writenopg_addrs[MAX_WRITE_NOPAGE_BUFFERS * MAX_THREADS / 10]; // harcorded
	// TODO use two hash tables
	DECLARE_HASHTABLE(every_rregion_hash, EVERY_REGION_HASH_BITS);
	rwlock_t every_rregion_hash_lock;
	DECLARE_HASHTABLE(every_wregion_hash, EVERY_REGION_HASH_BITS);
	rwlock_t every_wregion_hash_lock;

	/* determin which process run it is by threads */
	atomic_t barrier;
#endif
};

int TO_THE_OTHER_NID(void)
{
    if (my_nid == 0) { return 1; } else { return 0; }
}

#if GOD_VIEW
/***************
 * GOD VIEW - hash (SYS_REGION)
 */
unsigned long __god_region_hash(unsigned long curr_hash_id, int curr_cnt)
{
	return curr_hash_id + (curr_cnt << 16);
}

struct sys_omp_region *__sys_omp_region_hash(unsigned long region_hash_cnt_hash, unsigned long curr_hash_id, int curr_cnt)
{
	struct sys_omp_region *sys_region;
	hash_for_each_possible(sys_region_hash, sys_region, hentry, region_hash_cnt_hash) {
		if (sys_region->id == curr_hash_id &&
					sys_region->cnt == curr_cnt) {
			return sys_region;
		}
	}
	return NULL;
}
#if 0
	struct sys_omp_region *sys_region = kmalloc TODO
	spin_lock(&sys_region_hash_lock);
	found = __sys_omp_region_hash(hash);
	if (likely(!found))
		hash_add(sys_region_hash, &sys_region->hentry, addr);
	spin_unlock(&sys_region_hash_lock);
#endif

struct sys_omp_region *__add_sys_omp_region_hash(unsigned long sys_omp_region_id, unsigned long omp_hash, int region_cnt)
{
	struct remote_context *rc = current->mm->remote;
	struct sys_omp_region *sys_region = kzalloc(sizeof(*sys_region), GFP_KERNEL);
	BUG_ON(!sys_region);
	sys_region->id = omp_hash;
	sys_region->cnt = region_cnt;


//	sys_region->read_fault_cnt = 0;
	sys_region->read_cnt = 0;
	sys_region->read_max = 0;
	sys_region->read_min = 0;
	hash_init(sys_region->every_rregion_hash);
	rwlock_init(&sys_region->every_rregion_hash_lock);
	sys_region->skip_r_cnt = 0;
	sys_region->skip_r_cnt_msg_limit = 0;

//	sys_region->notminewrite_fault_cnt = 0;
	sys_region->writenopg_cnt = 0;
	sys_region->notminewrite_max = 0;
	sys_region->notminewrite_min = 0;
	hash_init(sys_region->every_wregion_hash);
	rwlock_init(&sys_region->every_wregion_hash_lock);
	sys_region->skip_w_cnt = 0;
	sys_region->skip_w_cnt_msg_limit = 0;

	/**/
	sys_region->slot_id = 0;
	spin_lock_init(&sys_region->region_lock);
	spin_lock_init(&sys_region->r_lock);
	spin_lock_init(&sys_region->w_lock);

	atomic_set(&sys_region->barrier, rc->threads_cnt);
#if 0
	/* performance reason - skip */
	memset(sys_region->read_cnt, 0,
			sizeof(*sys_region->read_cnt) * MAX_THREADS);		// hardcoded
	memset(sys_region->writenopg_cnt, 0,
			sizeof(*sys_region->writenopg_cnt) * MAX_THREADS);	// hardcoded

	memset(sys_region->read_addrs, 0,
			sizeof(**sys_region->read_addrs) *
			MAX_THREADS * MAX_READ_BUFFERS);					//hardcoded
	memset(sys_region->writenopg_addrs, 0,
			sizeof(**sys_region->writenopg_addrs) *
			MAX_THREADS * MAX_WRITE_NOPAGE_BUFFERS);			// hardcoded
#endif

//	write_lock(&sys_region_hash_lock); // deadlock
	hash_add(sys_region_hash, &sys_region->hentry, sys_omp_region_id);
//	write_unlock(&sys_region_hash_lock); // deadlock
	return sys_region;
}

/* not used */
struct sys_omp_region *__sys_omp_region_hash_add(unsigned long region_hash_cnt_hash, unsigned long omp_hash, int curr_cnt)
{
	struct sys_omp_region *sys_region = __sys_omp_region_hash(
						region_hash_cnt_hash, omp_hash, curr_cnt);
	if (!sys_region)
		sys_region = __add_sys_omp_region_hash(omp_hash, omp_hash, curr_cnt);

	return sys_region;
}

/****
 * GOD VIEW - functions
 * return: is_pf
 */
bool __start_begin_barrier(void)
{
	bool leader = false;
	struct remote_context *rc = current->mm->remote;
	int left_t = atomic_dec_return(&rc->barrier_begin);
	// select a leader to do prefetch?

	if (!left_t) { /* leader */ // rc/region
		leader = true;
	} else if (left_t > 0) {
		/* followers */
	} else {
		BUG();
	}
	return leader;
}

void __wait_begin_followers(struct remote_context *rc)
{
	/* Race Condition !!!!! wait all are in the wq */
	while (atomic_read(&rc->pendings_begin) != rc->threads_cnt - 1)
		CPU_RELAX;
}
void __end_begin_barrier(bool leader)
{
	struct remote_context *rc = current->mm->remote;
	if (leader) {
		/* all followers are all in the wq */
		PFVPRINTK("wait begin followers\n");
		//if (current->tso_region){printk("wait begin followers\n");}
		__wait_begin_followers(rc);
		PFVPRINTK("done begin followers\n");
		//if (current->tso_region){printk("done begin followers\n");}

		atomic_inc(&rc->pendings_begin);
		atomic_set(&rc->barrier_begin, rc->threads_cnt);
		wake_up_all(&rc->waits_begin);
		atomic_dec(&rc->pendings_begin);
		//printk("begin barrier leader done!!\n");
	} else {
		/* do sth efficient? */
		//while ();
		// __popcorn_end_end_barrier
		DEFINE_WAIT(wait);
		prepare_to_wait_exclusive(&rc->waits_begin, &wait, TASK_UNINTERRUPTIBLE);
//		printk("[%d] go to begin sleep %d\n",
//				current->pid, atomic_read(&rc->barrier_begin));
        atomic_inc(&rc->pendings_begin);
		io_schedule();
		finish_wait(&rc->waits_begin, &wait);
		atomic_dec(&rc->pendings_begin);
	}

#if CONSERVATIVE
	/* redudnat barrier - test - conservative */
	while (atomic_read(&rc->pendings_begin))
		CPU_RELAX;
	BUG_ON(atomic_read(&rc->pendings_begin) < 0);
#endif
}


/***************
 * Prefetch req
 */
void __wait_prefetch_req_done(int pending_pf_req)
{
	int i;
	struct remote_context *rc = current->mm->remote;
	if (!pending_pf_req) return;

	/* This prevent from RC */
	for (i = 0; i < pending_pf_req; i++)
		atomic_inc(&rc->pf_ongoing_cnt);

	PFVPRINTK("\t\tpf leader down\n");
	//if(!current->at_remote){printk("\t\tpf leader down\n");}
	while (atomic_read(&rc->pf_ongoing_cnt)) {
		CPU_RELAX; // TODO: BUG();
	}
	//if(!current->at_remote){printk("\t\tpf leader up\n");}
	PFVPRINTK("\t\tpf leader up\n");

#ifdef CONFIG_POPCORN_CHECK_SANITY
	BUG_ON(atomic_read(&rc->pf_ongoing_cnt));
#endif
}

// pf msg // very fast // not computation
void __send_pf_req(int pf_nr_pages, int send_id, struct prefetch_list_body *pf_reqs, struct pf_ongoing_map *pf_map, struct sys_omp_region *sys_region)
{
	struct remote_context *rc = current->mm->remote;
	remote_prefetch_request_t *req = pcn_kmsg_get(sizeof(*req));
	int size = (sizeof(*req) - (sizeof(*req->pf_reqs) * MAX_PF_REQ))
							+ (sizeof(*req->pf_reqs) * pf_nr_pages);
	BUG_ON(!req || !pf_nr_pages);

#ifdef CONFIG_POPCORN_CHECK_SANITY
	if (MAX_PF_REQ == pf_nr_pages)
		BUG_ON(size != sizeof(*req));
#endif

	pf_map->pf_req_id = send_id;
	pf_map->pf_list_size = pf_nr_pages;
    spin_lock(&rc->pf_ongoing_lock);
	list_add_tail(&pf_map->list, &rc->pf_ongoing_list);
    spin_unlock(&rc->pf_ongoing_lock);

	req->origin_pid = current->pid;
//    req->remote_pid = current->origin_pid;
    req->remote_pid = rc->remote_tgids[TO_THE_OTHER_NID()];

	req->pf_req_id = send_id;
	req->pf_list_size = pf_nr_pages;
	memcpy(req->pf_reqs, pf_reqs, sizeof(struct prefetch_list_body) * pf_nr_pages);

	req->god_omp_hash = __god_region_hash(sys_region->id, sys_region->cnt);

	pcn_kmsg_post(PCN_KMSG_TYPE_REMOTE_PREFETCH_REQUEST,
							TO_THE_OTHER_NID(), req, size);

//	atomic_inc(&rc->pf_ongoing_cnt);

#if 1
	PFPRINTK("\t\t\t->->#%d[/%d] 0x%lx ->->%d hash 0x%lx size %d fence %lu\n",
									send_id, pf_nr_pages,
									pf_reqs[pf_nr_pages - 1].addr,
									TO_THE_OTHER_NID(),
					__god_region_hash(sys_region->id, sys_region->cnt), size,
					current->tso_fence_cnt);
#endif

#if 0
#define MAX_NAME 255
	char str[MAX_NAME]; int i, ofs = 0;
	memset(str, 0, MAX_NAME);
	ofs += snprintf(str, MAX_NAME, "%s", "\t\taddrs: ");
	for (i = 0; i < pf_nr_pages; i++) {
		ofs += snprintf(str + ofs, MAX_NAME, "%s", "0x");
		ofs += snprintf(str + ofs, MAX_NAME, "%lx", req->pf_reqs[i].addr);
		ofs += snprintf(str + ofs, MAX_NAME, "%s", " ");
	}
	printk("%s\n", str);
#endif
}

extern void handle_prefetch(remote_prefetch_request_t *req);
//static int handle_remote_prefetch_request(struct pcn_kmsg_message *msg)
static void process_remote_prefetch_request(struct work_struct *work)
{
	//remote_prefetch_request_t *req = (remote_prefetch_request_t *)msg;
    START_KMSG_WORK(remote_prefetch_request_t, req, work);
//	BUG_ON(tsk->at_remote);
#ifdef CONFIG_POPCORN_CHECK_SANITY
	BUG_ON(!req);
#endif
	PFVPRINTK("\t\t<-got req hash 0x%lx #%d\n",
				req->god_omp_hash, req->pf_req_id);



	handle_prefetch(req);



#if 0
    if (tsk->at_remote) {
		// TODO new
	} else {
		/* Only support remote prefetch */
//		__handle_prefetch_at_origin(from_nid, req->remote_pid, req->origin_pid,
//										req->pf_list_size, req->pf_req_id,
//										(struct prefetch_body*)&req->pf_list);
	}
#endif
	PFVPRINTK("\t\t<-dAone req hash 0x%lx #%d\n",
				req->god_omp_hash, req->pf_req_id);
    END_KMSG_WORK(req);
	//pcn_kmsg_done(req);
	//return 0;
}

extern struct remote_context *pfpg_fixup(void *msg, struct pcn_kmsg_rdma_handle *pf_handle);
//static int handle_remote_prefetch_response(struct pcn_kmsg_message *msg)
static void process_remote_prefetch_response(struct work_struct *work)
{
	//remote_prefetch_response_t *res = (remote_prefetch_response_t *)msg;
    START_KMSG_WORK(remote_prefetch_response_t, res, work);
	struct remote_context *rc;
	PFVPRINTK("\t\t<-touched 0x%lx #%d\n",
				res->god_omp_hash, res->pf_req_id);
	rc = pfpg_fixup(res, NULL);

	atomic_dec(&rc->pf_ongoing_cnt);

	PFVPRINTK("\t\t<-done reqs hash 0x%lx #%d\n",
				res->god_omp_hash, res->pf_req_id);
    END_KMSG_WORK(res);
	//pcn_kmsg_done(res);
	//return 0;
}

extern struct fault_handle *select_prefetch_page(unsigned long addr);
// return: pending_pf_req
int __select_prefetch_pages_send(struct sys_omp_region *sys_region)
{
	int bkt;
	struct fault_info *fi;
	struct hlist_node *tmp;
	int pf_nr_pages = 0;
	int pending_pf_req = 0;
	struct pf_ongoing_map *pf_map;
	int r_cnt = sys_region->read_cnt; // just for debuging
	struct prefetch_list_body pf_reqs[MAX_PF_REQ];
	int sent_cnt = 0;

#ifdef CONFIG_POPCORN_CHECK_SANITY
	BUG_ON(atomic_read(&current->mm->remote->pf_ongoing_cnt));
#endif

	// upper bound: sys_region->read_cnt
	hash_for_each_safe(sys_region->every_rregion_hash, bkt, tmp, fi, hentry) {
		unsigned long addr = fi->addr;
		struct fault_handle *fh = select_prefetch_page(addr);
		r_cnt--;
		if (!fh)
			continue;

		sent_cnt++;
		/* on going req map */
		if (!pf_nr_pages)
			pf_map = kzalloc(sizeof(*pf_map), GFP_KERNEL);
		BUG_ON(!pf_map);
		pf_map->fh[pf_nr_pages] = fh;
		pf_map->addr[pf_nr_pages] = addr;

		/* preparing msg */
		pf_reqs[pf_nr_pages].addr = addr;
		pf_nr_pages++;
		// load to msg 1 ~ 7 (0-6)

		if (pf_nr_pages >= MAX_PF_REQ) { // >= 7 ([0-6]) send
			__send_pf_req(pf_nr_pages, pending_pf_req, pf_reqs, pf_map, sys_region);
			pending_pf_req++;
			pf_nr_pages = 0;
#if SKIP_MEM_CLEAN
			/* performance reason - skip */
			memset(pf_reqs, 0,
				sizeof((sizeof(struct prefetch_list_body) * MAX_PF_REQ)));
#endif
		}

		/* msg limitation */
		if (pending_pf_req >= ARM_THREADS - 1 - MSG_ROOM_FOR_OTHER_MSG)
			break;
	}
	if (pf_nr_pages) { // 1 ~ 6
		__send_pf_req(pf_nr_pages, pending_pf_req, pf_reqs, pf_map, sys_region);
		pending_pf_req++;
	}

	if (pending_pf_req >= ARM_THREADS - 1 - MSG_ROOM_FOR_OTHER_MSG) {
		sys_region->skip_r_cnt_msg_limit += r_cnt;
#if PERF_FULL_BULL_WARN
		PCNPRINTK_ERR("*** skip - msg limit ***\n");
#endif
		goto out; // skip every_wregion_hash
	}


	hash_for_each_safe(sys_region->every_wregion_hash, bkt, tmp, fi, hentry) {
		//TODO every_wregion_hash
		//TODO every_wregion_hash
		//TODO every_wregion_hash
		//TODO every_wregion_hash
		//TODO every_wregion_hash
		//TODO every_wregion_hash
	}
	//TODO every_wregion_hash

out:
	PFPRINTK("-->--> sent_cnt %d (%d msgs) == r_cnt %d (w_cnt %d) hash 0x%lx -->-->\n",
				sent_cnt, pending_pf_req,
				sys_region->read_cnt, sys_region->writenopg_cnt,
				__god_region_hash(sys_region->id, sys_region->cnt));
	return pending_pf_req;
}

void __prefetch(struct sys_omp_region *sys_region)
{
	int pending_pf_req;
	/* TODO: stop-my-world prefetch (others may not stopped!!!) */
#if STRONG_CHECK_SANITY
	struct hlist_node *tmp;
	struct fault_info *fi;
	int bkt, r_cnt = 0, w_cnt = 0;

	BUG_ON(atomic_read(&current->mm->remote->pf_ongoing_cnt));

	hash_for_each_safe(sys_region->every_rregion_hash, bkt, tmp, fi, hentry) {
		r_cnt++;
	}

	hash_for_each_safe(sys_region->every_wregion_hash, bkt, tmp, fi, hentry) {
		w_cnt++;
	}
	BUG_ON(r_cnt != sys_region->read_cnt);
	BUG_ON(w_cnt != sys_region->writenopg_cnt);
#endif

	PFPRINTK("[%d] 0x%lx #%d: hash 0x%lx prefetch r_cnt %d w_cnt %d fence %lu\n",
			current->pid, sys_region->id, sys_region->cnt,
			__god_region_hash(sys_region->id, sys_region->cnt),
			sys_region->read_cnt, sys_region->writenopg_cnt,
			current->tso_fence_cnt);

	/* issue prefetchs & wait till all done */
	pending_pf_req = __select_prefetch_pages_send(sys_region);
	__wait_prefetch_req_done(pending_pf_req);
}

bool __may_prefetch(struct sys_omp_region *sys_region)
{
	//if ((sys_region->read_cnt + sys_region->writenopg_cnt) < PREFETCH_THRESHOLD ||
	if (sys_region->read_cnt < PREFETCH_THRESHOLD ||
		sys_region->read_cnt <= 0) {
		return false;
	}
	__prefetch(sys_region);
	return true;
}

struct sys_omp_region *__god_view_prefetch(struct remote_context *rc, unsigned long omp_hash, int region_cnt)
{
	unsigned long region_hash_cnt_hash = __god_region_hash(omp_hash, region_cnt);
	struct sys_omp_region *sys_region =
		__sys_omp_region_hash(region_hash_cnt_hash, omp_hash, region_cnt);
	if (!sys_region)
		return NULL; // first run // previous didn't record
	BUG_ON(!region_hash_cnt_hash || !omp_hash);

	/* god view has prepared - god view prefetch */
	if(!__may_prefetch(sys_region)) {
#if 1
		PFPRINTK("[%d] 0x%lx #%d: hash 0x%lx NO PF r_cnt %d w_cnt %d fence %lu\n",
				current->pid, sys_region->id, sys_region->cnt,
				__god_region_hash(sys_region->id, sys_region->cnt),
				sys_region->read_cnt, sys_region->writenopg_cnt,
				current->tso_fence_cnt);
#endif
	}

	return sys_region;
}
/* every thread save its addr[] to the region */

/***
 * handle prefetch req
 */


/* all */
void __ondemand_si_region_create(unsigned long omp_hash, int region_cnt)
{
	unsigned long region_hash_cnt_hash = __god_region_hash(omp_hash, region_cnt);
	struct sys_omp_region *sys_region =
		__sys_omp_region_hash(region_hash_cnt_hash, omp_hash, region_cnt);
	if (!region_cnt) // don't create for cnt because of omp_region sementic
		return;
	if (!omp_hash)
		return;

	if (!sys_region) { /* create */
		sys_region =
			__add_sys_omp_region_hash(region_hash_cnt_hash, omp_hash, region_cnt);
		//printk("create hash 0x%lx 0x%lx %d - %p\n",
		//		region_hash_cnt_hash, omp_hash, region_cnt, sys_region);
#if STRONG_CHECK_SANITY
		BUG_ON(!__sys_omp_region_hash(region_hash_cnt_hash, omp_hash, region_cnt));
#endif
		//BUG_ON(!omp_hash);
		//BUG_ON(!region_hash_cnt_hash);
	}
}

void __dbg_si_addrs(unsigned long omp_hash, int region_cnt)
{
	unsigned long region_hash_cnt_hash = __god_region_hash(omp_hash, region_cnt);
	struct sys_omp_region *sys_region =
		__sys_omp_region_hash(region_hash_cnt_hash, omp_hash, region_cnt);
//	if (!region_cnt) // covered by !sys_region
//		return;
//	printk("dbg 0x%lx 0x%lx %d - %p\n",
//			region_hash_cnt_hash, omp_hash, region_cnt, sys_region);
	if (!sys_region)
		return;
	BUG_ON(!region_hash_cnt_hash);

	//if (sys_region->read_cnt + sys_region->writenopg_cnt >= PREFETCH_THRESHOLD &&
	if (sys_region->read_cnt >= PREFETCH_THRESHOLD &&
		sys_region->read_cnt > 0) {
#if 1
		PFPRINTK("[%d] 0x%lx #%d: hash 0x%lx pg_cnt r %d w %d fence %lu\n",
				current->pid, sys_region->id, region_cnt,
				__god_region_hash(sys_region->id, sys_region->cnt),
				sys_region->read_cnt, sys_region->writenopg_cnt,
				current->tso_fence_cnt);
#endif
	}
}

void __show_god_mistake(struct remote_context *rc)
{
#if STATIS // put for all variables
	printk("god view mistake wrong_hist %d\n", rc->wrong_hist);
	printk("god view mistake wrong_hist_no_vma %d\n", rc->wrong_hist_no_vma);
#endif
}

void __show_si_addrs(void)
{
#if STATIS // put for all variables
	struct hlist_node *tmp;
	struct sys_omp_region *sys_region;
	int bkt, region_cnt = 0, r_cnt = 0, w_cnt = 0;
	int skip_r_cnt = 0, skip_w_cnt = 0;
	int skip_r_cnt_msg_limit = 0, skip_w_cnt_msg_limit = 0;
	hash_for_each_safe(sys_region_hash, bkt, tmp, sys_region, hentry) {
		r_cnt += sys_region->read_cnt;
		w_cnt += sys_region->writenopg_cnt;

		skip_r_cnt += sys_region->skip_r_cnt;
		skip_w_cnt += sys_region->skip_w_cnt;

		skip_r_cnt_msg_limit += sys_region->skip_r_cnt_msg_limit;
		skip_w_cnt_msg_limit += sys_region->skip_w_cnt_msg_limit;
		region_cnt++;
	}
	printk("god view region_cnt %d pg_cnt r %d w %d "
				"skip r %d w %d msg limit skip r %d w %d\n",
						region_cnt, r_cnt, w_cnt, skip_r_cnt, skip_w_cnt,
						skip_r_cnt_msg_limit, skip_w_cnt_msg_limit);
	printk("god view region_cnt %d pg_cnt r %d w %d "
				"skip r %d w %d msg limit skip r %d w %d\n",
						region_cnt, r_cnt, w_cnt, skip_r_cnt, skip_w_cnt,
						skip_r_cnt_msg_limit, skip_w_cnt_msg_limit);
	printk("god view region_cnt %d pg_cnt r %d w %d "
						"skip r %d w %d "
						"msg limit skip r %d w %d\n",
						region_cnt, r_cnt, w_cnt,
						skip_r_cnt, skip_w_cnt,
						skip_r_cnt_msg_limit, skip_w_cnt_msg_limit);
	printk("TODO: 123\n");
	printk("TODO: wq implmementation\n");
	printk("TODO: orfer wq:send -> post\n");
#endif
}
#endif

/***************
 * SMART_REGION_DBG
 */
void __region_total_cnt_inc(struct omp_region *region)
{
#if SMART_REGION_DBG
	atomic_inc(&region->total_cnt);
#endif
}

void __region_skip_cnt_inc(struct omp_region *region)
{
#if SMART_REGION_DBG
	atomic_inc(&region->skip_cnt);
#endif
}

void inline ______smart_region_debug(struct remote_context *rc, struct omp_region *pos)
{
#if SMART_REGION_DBG
	printk("[0x%lx] %s:%d type 0x%lx in_reg_inv_sum %d in_reg_conf_sum %d "
			" (Regions) %d "
			"ompr->total_R %d ompr->skip_R %d [ReadFault] cnt %d\n",
			//"ompr->total_R %d/t %d ompr->skip_R %d/t %d [ReadFaulf] cnt %d\n",
			pos->id, pos->name, pos->line,
			pos->type, pos->in_region_inv_sum,
			pos->in_region_conflict_sum, pos->cnt,
#ifdef CONFIG_X86_64
			//atomic_read(&pos->total_cnt),
			atomic_read(&pos->total_cnt) / X86_THREADS,
			//atomic_read(&pos->skip_cnt),
			atomic_read(&pos->skip_cnt) / X86_THREADS,
#else
			//atomic_read(&pos->total_cnt),
			atomic_read(&pos->total_cnt) / ARM_THREADS,
			//atomic_read(&pos->skip_cnt),
			atomic_read(&pos->skip_cnt) / ARM_THREADS,
#endif
			pos->read_fault_cnt
			);
#endif
#ifdef CONFIG_X86_64
//	BUG_ON(rc->threads_cnt != X86_THREADS); // use rc->threads_cnt
#else
//	BUG_ON(rc->threads_cnt != ARM_THREADS); // use rc->threads_cnt
#endif
}

void ____smart_region_debug(struct remote_context *rc, int larger)
{
	int bkt;
	struct hlist_node *tmp;
	struct omp_region *pos;
	hash_for_each_safe(rc->omp_region_hash, bkt, tmp, pos, hentry) {
		if (larger) {
			if (pos->in_region_inv_sum >= 100)
				______smart_region_debug(rc, pos);
		} else {
			if (pos->in_region_inv_sum < 100 )
				______smart_region_debug(rc, pos);
		}
	}
}

void __smart_region_debug(struct remote_context *rc)
{
#if SMART_REGION_DBG
	printk("=============== SMART REGION DBG START ===============\n");
	____smart_region_debug(rc, 0);
	printk("============ SMART REGION DBG IMPORTANT START ============\n");
	____smart_region_debug(rc, 1);
	printk("=============== SMART REGION DBG END ===============\n");
#endif
}

void clean_omp_region_hash(struct remote_context *rc)
{
	int bkt;
	struct hlist_node *tmp;
	struct omp_region *pos;

	__smart_region_debug(rc);
#if GOD_VIEW
	__show_si_addrs();
	__show_god_mistake(rc);
#endif

	hash_for_each_safe(rc->omp_region_hash, bkt, tmp, pos, hentry) {
#if DECISION_PREFETCH_STATIS
		/* clean read fault */
		int rf_bkt;
		struct hlist_node *rf_tmp;
		struct readfault_info *rf;
		write_lock(&pos->per_region_hash_lock);
		hash_for_each_safe(pos->per_region_hash, rf_bkt, rf_tmp, rf, hentry) {
			hlist_del(&rf->hentry);
			kfree(rf);
		}
		write_unlock(&pos->per_region_hash_lock);
#endif


		hlist_del(&pos->hentry);
		kfree(pos);
	}
#if SMART_REGION_DBG
#endif
	BUG_ON(!hash_empty(rc->omp_region_hash));
}


/***************
 * OMP region hash
 * opt to prealloc
 */
struct omp_region *__add_omp_region_hash(struct remote_context *rc, unsigned long omp_region_id)
{
	struct omp_region *region = kzalloc(sizeof(*region), GFP_KERNEL);
	region->id = omp_region_id;
	region->type = RCSI_UNKNOW;
	region->cnt = 0;
#if SMART_REGION_DBG
	atomic_set(&region->total_cnt, 1);
	atomic_set(&region->skip_cnt, 0);
	region->vain_cnt = 0;
	region->name[0] = 0;
	region->line = 0;
#endif
	region->read_fault_cnt = 0;
	region->read_max = 0;
	region->read_min = 0;
	region->in_region_inv_sum = 0;
	region->in_region_conflict_sum = 0;
	region->ht_selector = false;
	hash_init(region->per_region_hash);
	rwlock_init(&region->per_region_hash_lock);

	hash_add(rc->omp_region_hash, &region->hentry, omp_region_id);
	return region;
}

struct omp_region *__omp_region_hash(struct remote_context *rc, unsigned long omp_region_id)
{
	struct omp_region *pos, *region = NULL;
	read_lock(&rc->omp_region_hash_lock);
	hash_for_each_possible(rc->omp_region_hash, pos, hentry, omp_region_id) {
		if (pos->id == omp_region_id) {
			region = pos;
			break;
		}
	}
	read_unlock(&rc->omp_region_hash_lock);
	return region;
}

/* must return !NULL */
struct omp_region *__omp_region_hash_add(struct remote_context *rc, unsigned long omp_hash)
{
	struct omp_region *region = __omp_region_hash(rc, omp_hash);
	if (!region)
		region = __add_omp_region_hash(rc, omp_hash);

	/* use region to do something */
	return region;
}

/* tso_end_barrier(begin) & leader */ // can delay but before rc region info cleaned
struct omp_region *__analyze_region(struct remote_context *rc)
{
	unsigned long omp_hash = current->tso_region_id;
	struct omp_region *region = __omp_region_hash_add(rc, omp_hash);
#if PREFETCH
	unsigned long region_hash_cnt_hash =
				__god_region_hash(current->omp_hash, current->region_cnt);
	struct sys_omp_region *sys_region =
				__sys_omp_region_hash(region_hash_cnt_hash,
										current->omp_hash, current->region_cnt);
#endif
	BUG_ON(!omp_hash);
	BUG_ON(!region);

#if SMART_REGION_DBG
	region->in_region_inv_sum += rc->inv_cnt;
#endif
	if (!(region->type & RCSI_VAIN)) {
		if (!sys_region) return region;
		if (rc->inv_cnt < VAIN_THRESHOLD
#if PREFETCH
			//&& sys_region->read_cnt + sys_region->writenopg_cnt
			&& sys_region->read_cnt < PREFETCH_THRESHOLD
#endif
			)
		{
			++region->vain_cnt;
			if (region->vain_cnt >= VAIN_REGION_REPEAT_THRESHOLD) {
				region->type |= RCSI_VAIN; // update local
#if SMART_REGION_PERF_DBG
//				printk("[0x%lx] %d change to VAIN 0x%lx %d region(%s)\n",
//						region->id, rc->inv_cnt,
//						region->type, region->cnt,
//						current->tso_region?"O":"X");
#endif
			}
			/* will update the remote to double check by handshake */
		} else {
			region->vain_cnt = 0;
			// TODO: determin what type REPEAT/SERIAL (need at lease two logs) rc->inv_addrs[rc->inv_cnt]
		}
	} // else done
	return region;
}

/* multi thread concurrent */
void __try_flip_region_type(struct remote_context *rc)
{
	if (current->tso_wr_cnt > VAIN_THRESHOLD) {
		++current->tso_benefit_cnt;
		if (current->tso_benefit_cnt >= BENEFIT_REGION_REPEAT_THRESHOLD) {
			struct omp_region *region =
					__omp_region_hash(rc, current->tso_region_id);
			BUG_ON(!region);
			if (region->type & RCSI_VAIN) {
				//region->type &= ~RCSI_VAIN;
				// TODO BUG TODO BUG TODO BUG TODO BUG TODO BUG TODO BUG
				// TODO BUG TODO BUG TODO BUG TODO BUG TODO BUG TODO BUG
				// TODO BUG TODO BUG TODO BUG TODO BUG TODO BUG TODO BUG
#if SMART_REGION_DBG
				printk("[0x%lx] %llu clean RCSI_VAIN %d \n",
							region->id, current->tso_wr_cnt,
							current->tso_benefit_cnt);
#endif
				/* yes: eul */
				/* small: CG */
				/* no: lud */
				/* enter next begion WARN: skew will hang */
			}
		}
	} else {
		current->tso_benefit_cnt = 0;
	}
	current->smart_skip_cnt += current->skip_wr_cnt;
	current->skip_wr_cnt = 0;
}


/************
 * Prefetch
 */
/* TODO: add writefault + !page_mine */
/* TODO: add writefault + !page_mine */
/* TODO: add writefault + !page_mine */
/* TODO: add writefault + !page_mine */
/* TODO: add writefault + !page_mine */
bool rcsi_readfault_collect(struct task_struct *tsk, unsigned long addr)
{
#if DECISION_PREFETCH_STATIS
	struct omp_region *region;
	struct readfault_info *rf;
	unsigned long omp_hash = tsk->tso_region_id;
	struct remote_context *rc = tsk->mm->remote;
	if (!tsk->tso_region || !omp_hash)
		return false;

	region = __omp_region_hash(rc, omp_hash);
	if (!region)
		return false;

	/* peek - relaxed */
	hash_for_each_possible(region->per_region_hash, rf, hentry, addr)
		if (rf->addr == addr)
			return false;

	rf = kzalloc(sizeof(*rf), GFP_KERNEL); /* TODO perf optimize bitmap? */
	BUG_ON(!rf);
	rf->addr = addr;
	write_lock(&region->per_region_hash_lock);
	region->read_fault_cnt++; /* current problem is this # too small */
	/* TODO BUG() -  first few addr are not the most common?  */
	/* TODO BUG() -  first few addr are not the most common?  */
	/* TODO BUG() -  first few addr are not the most common?  */
	if (region->read_max == 0 ||
			(addr > region->read_max &&
			(addr - region->read_max) < 0x7fff00000000))
		region->read_max = addr;
	if (region->read_min == 0 ||
			(addr < region->read_min &&
			(region->read_min - addr) < 0x7fff00000000))
		region->read_min = addr;
	hash_add(region->per_region_hash, &rf->hentry, addr);
	write_unlock(&region->per_region_hash_lock);
#if SMART_REGION_PERF_DBG
//	printk("DBG RF [0x%lx] 0x%lx max 0x%lx min 0x%lx\n",
//				omp_hash, addr, region->read_max, region->read_min);
#endif

#endif
	return true;
}


/***************
 * RCSI + MEM pool has
 */
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
	//current->buffer_inv_addrs[current->tso_wr_cnt] = addr;
	//current->tso_wr_cnt++;
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

/********
 * Duplication
 */
struct sys_omp_region *__si_inc_common(struct task_struct *tsk)
{
#if GOD_VIEW
	unsigned long omp_hash = tsk->omp_hash;
	int region_cnt = tsk->region_cnt;
	unsigned long region_hash_cnt_hash = __god_region_hash(omp_hash, region_cnt);
	struct sys_omp_region *sys_region =
		__sys_omp_region_hash(region_hash_cnt_hash, omp_hash, region_cnt);
//	if (!region_cnt) // first // covered by !sys_reigon
//		return NULL;;
	if (!tsk->tso_region)
		return NULL;
	if (!sys_region)
		return NULL;
	BUG_ON(!region_hash_cnt_hash);
	BUG_ON(tsk != current);

	return sys_region;
#else
	return NULL;
#endif
}

void si_read_inc(struct task_struct *tsk, unsigned long addr)
{
#if GOD_VIEW
	struct fault_info *fi;
	struct sys_omp_region *sys_region = __si_inc_common(tsk);
	if (!sys_region)
		return;

	/* save */
	spin_lock(&sys_region->r_lock);
	if (sys_region->read_cnt >=
			sizeof(sys_region->read_addrs) /
			sizeof(*sys_region->read_addrs)) {
		sys_region->skip_r_cnt++;
#if PERF_FULL_BULL_WARN
		printk("si_r_addrs_buf full\n");
#endif
		goto out;
	}

	// peek
	hash_for_each_possible(sys_region->every_rregion_hash, fi, hentry, addr)
		if (fi->addr == addr)
			goto out;

	fi = kmalloc(sizeof(*fi), GFP_ATOMIC);
	BUG_ON(!fi);
	fi->addr = addr;
	hash_add(sys_region->every_rregion_hash, &fi->hentry, addr);
	sys_region->read_addrs[sys_region->read_cnt] = addr; /* g_arry */ //kill
	sys_region->read_cnt++;

out:
	spin_unlock(&sys_region->r_lock);
#endif
}

void si_writenopg_inc(struct task_struct *tsk, unsigned long addr)
{
#if GOD_VIEW
	struct fault_info *fi;
	struct sys_omp_region *sys_region = __si_inc_common(tsk);
	if (!sys_region)
		return;

	/* save */
	spin_lock(&sys_region->w_lock);
	if (sys_region->writenopg_cnt >=
			sizeof(sys_region->writenopg_addrs) /
			sizeof(*sys_region->writenopg_addrs)) {
		sys_region->skip_w_cnt++;
#if PERF_FULL_BULL_WARN
		printk("si_r_addrs_buf full\n");
#endif
		goto out;
	}

	// peek
	hash_for_each_possible(sys_region->every_wregion_hash, fi, hentry, addr)
		if (fi->addr == addr)
			goto out;

	fi = kmalloc(sizeof(*fi), GFP_ATOMIC); // pg fault path
	BUG_ON(!fi);
	fi->addr = addr;
	hash_add(sys_region->every_wregion_hash, &fi->hentry, addr);
	sys_region->writenopg_addrs[sys_region->writenopg_cnt] = addr; /* g_arry */ // kill
	sys_region->writenopg_cnt++;

out:
	spin_unlock(&sys_region->w_lock);
#endif
}

/* ugly */
extern void __print_pids(struct remote_context *rc);
extern pte_t *get_pte_at(struct mm_struct *mm, unsigned long addr, pmd_t **ppmd, spinlock_t **ptlp);
/*************************
 * for barriers/fence
 */
#if 0
void __clean_perthread_inv_buf(int inv_cnt)
{	/* out-dated */
	int j;
	for (j = 0; j < inv_cnt; j++)
		current->buffer_inv_addrs[j] = 0;
}
#endif

#if HASH_GLOBAL
/* serial phase */
void __clean_global_rcsi(struct remote_context *rc)
{
	int bkt;
	struct rcsi_work *pos;
	struct hlist_node *tmp;
	/* Attention: lock order */
	/* clean hash table - TODO func */
	//spin_lock(&rcsi_hash_lock);
	hash_for_each_safe(rcsi_hash, bkt, tmp, pos, hentry) { /* iter objs */
		pos->addr = 0;
		if (!NOCOPY_NODE) { // SKIP_MEM_CLEAN can be skipped right? perf
			memset(pos->paddr, 0, PAGE_SIZE); /* TODO: perf critical */
		}
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

/* read fault - serial */
void __clean_rcsi_read_fault(struct remote_context *rc)
{
#if DECISION_PREFETCH_STATIS
	int bkt, read_fault_cnt = 0;
	struct readfault_info *rf;
	struct hlist_node *tmp;
#endif
	unsigned long omp_hash = current->tso_region_id;
	struct omp_region *region = __omp_region_hash(rc, omp_hash);
#if SMART_REGION_DBG
	int predict_read_cnt = (region->read_max - region->read_min) / PAGE_SIZE;
	unsigned long max = region->read_max;
	unsigned long min = region->read_min;
#endif
	region->read_max = 0;
	region->read_min = 0;
	region->ht_selector = !region->ht_selector;
	SMRPRINTK("[0x%lx] selec %d\n", omp_hash, region->ht_selector);

#if DECISION_PREFETCH_STATIS
	/* TODO use selector to choose hash table */
	/* remove all read fault addrs from this this region */
	//writelock(&rc->omp_region_hash_lock);
	hash_for_each_safe(region->per_region_hash, bkt, tmp, rf, hentry) {
		read_fault_cnt++;
		hlist_del(&rf->hentry);
	}
	//write_unlock(&rc->omp_region_hash_lock);
	BUG_ON(!hash_empty(region->per_region_hash));
#endif

#if SMART_REGION_DBG
	printk("[0x%lx] select(%s) vain %d rc->inv_cnt(per region) %d "
			"max 0x%lx min 0x%lx "
			"***read_fault_cnt %d predict range %d***\n",
			omp_hash, region->ht_selector == true ? "O" : "X",
			region->vain_cnt, rc->inv_cnt, max, min,
			read_fault_cnt, predict_read_cnt);
	// predict by range:
	// good:
	// bad: lud, eul
	// eul 1 reg ~236
#endif
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
#if PREFETCH
	__clean_rcsi_read_fault(rc);
#endif
	__clean_global_rcsi(rc);
#else
	/* clean all for each barrier/fence */
	__clean_global_inv(rc);
#endif
#endif
}


//PCN_KMSG_TYPE_PAGE_MERGE_DONE_REQUEST /* tell remote */ /* handshak */
/* [leader all done/handshak] -> remote */
static void	__sned_handshake(struct remote_context *rc, int nid, struct omp_region *region)
{
#if 0
	remote_baiier_done_request_t req = {
        .origin_pid = rc->remote_tgids[nid],
		.remote_region_type = region->type,
    };

	/* tell remote */ /* handshak */
    pcn_kmsg_send(PCN_KMSG_TYPE_PAGE_MERGE_DONE_REQUEST,
								nid, &req, sizeof(req));
#endif
	remote_baiier_done_request_t *req = pcn_kmsg_get(sizeof(*req));
    req->origin_pid = rc->remote_tgids[nid],
	req->remote_region_type = region->type,

	/* tell remote */ /* handshak */
    pcn_kmsg_post(PCN_KMSG_TYPE_PAGE_MERGE_DONE_REQUEST,
								nid, req, sizeof(*req));
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
	rc->sys_rw_cnt += rc->inv_cnt;

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

	while (atomic_read(&rc->scatter_pendings))
		CPU_RELAX;

	BUG_ON(atomic_read(&rc->scatter_pendings)); // remove

	/* general */
	/* scatters - send - even send when 0 */
	if (local_wr_cnt == 0) { /* special zero case 0/0 */
		iter = 0; /* total == 0 iter == 0 */
		total_iter = 0;
		atomic_inc(&rc->scatter_pendings);
		// current->tso_nobenefit_region_cnt++; /* TODO: use rc and function */
	} else if (local_wr_cnt > 0) {
		int i;
		iter++; /* total > 0 iter start from 1 */
		total_iter = ((local_wr_cnt - 1) / MAX_WRITE_INV_BUFFERS) + 1;
		zero_case = false;

		BUG_ON(!total_iter); // remove
		for (i = 0; i < total_iter; i++)
			atomic_inc(&rc->scatter_pendings);
	} else {
		BUG();
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
		WSYPRINTK("\t  -> MERGE sent l#%d %d/%d\n",
					rc->local_merge_id, iter, total_iter);

		iter++;
		sent_cnt += single_sent;

		if (zero_case)	/* redundant? */
			break;
	} while (sent_cnt < local_wr_cnt);

	WSYPRINTK("\t  -> MERGE sent go to sleep l id %d wait %d mg res msgs ws %p\n",
										rc->local_merge_id, total_iter, ws);
	/* wait until the last scatter requestion done + diff_req__from_remote done */
	wait_at_station(ws);
	kfree(req); /* conservative */
}

extern int do_locally_inv_page(struct task_struct *tsk, unsigned long addr);
/* optimize: overwrite the vma buffer for performance */
static void __make_diff(char *new, char *origin, page_diff_apply_request_t *req)
{
	int i;
#if STRONG_CHECK_SANITY
	int good = 0;
#endif
	for (i = 0; i < PAGE_SIZE; i++)
		*(req->diff_page + i) = *(new + i) ^ *(origin + i);

#if STRONG_CHECK_SANITY
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
#if STRONG_CHECK_SANITY
	int i, good = 0;
#endif
	BUG_ON(!req);
	BUG_ON(!conflict_addr);
#if STRONG_CHECK_SANITY
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

#if CONSERVATIVE
	get_page(page);
	paddr = kmap(page);
#else
	paddr = kmap_atomic(page);
#endif
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

#if CONSERVATIVE
	kunmap(page);
	put_page(page);
#else
	kunmap_atomic(paddr);
#endif

	pte_unmap(pte);
	up_read(&tsk->mm->mmap_sem);

#if STRONG_CHECK_SANITY
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

	/* (xxx) make sure the send order doesn't matter */
	/* order issue has been handled by __wait_diffs_done() */
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

#if CONSERVATIVE
	get_page(page);
	paddr = kmap(page);
#else
	paddr = kmap_atomic(page);
#endif
	BUG_ON(!paddr);

	__apply_diff(paddr, req->diff_page);

#if CONSERVATIVE
	kunmap(page);
	put_page(page);
#else
	kunmap_atomic(paddr);
#endif

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

	__do_apply_diff(req, tsk);

	/* ownership */
	// TODO BUG will hang......cause sync proble.........why
	//				sync_set_page_owner(0, tsk->mm, req->diff_addr);
	//				sync_clear_page_owner(1, tsk->mm, req->diff_addr);
	// this msg may/ may not happen. So fix here is not realistic <- WRONG
	// only for ww case // normally inv case has been handled in the first place.
	// this is the 2nd bandadge for special cases

	atomic_dec(&rc->doing_diff_cnt);
	atomic_inc(&rc->req_diffs);
	atomic_inc(&rc->sys_ww_cnt);
	DIFFPRINTK("[%d/%5d] locally applying diff 0x%lx\n",
			tsk->pid, atomic_read(&rc->req_diffs), req->diff_addr);

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

#if STRONG_CHECK_SANITY
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
				if (my_nid == 0 && NOCOPY_NODE == 0) { /* only origin maintain pip */
					/* Not True */

					/* TODO try to make sure the ownership again */
					// do_locally_own_page()
					sync_set_page_owner(0, tsk->mm, req_addr);
					sync_clear_page_owner(1, tsk->mm, req_addr);

					/* or can do it in handle_page_apply_diff_request() */
				} else if (my_nid == 0 && NOCOPY_NODE == 0) {
					/* TODO: test  TODO: test  TODO: test  TODO: test */
					/* TODO: test  TODO: test  TODO: test  TODO: test */
					/* TODO: test  TODO: test  TODO: test  TODO: test */
					sync_set_page_owner(1, tsk->mm, req_addr);
					sync_clear_page_owner(0, tsk->mm, req_addr);
				}
			}
#endif
		}
	}
#else
	BUG_ON("Not supprot for PER-THREAD list");
#endif
	/* cross check */
	rc->remote_sys_ww_cnt += conflict_cnt; /* ww_cnt is a bad name */
	atomic_add(conflict_cnt, &rc->diffs);
	smp_wmb();
	// only work at origin
	//printk("test id %d\n", tsk->tso_region_id);
	if (tsk->tso_region_id) {
		struct omp_region *region = __omp_region_hash(rc, tsk->tso_region_id);
		if (region) // only work for NOCOPY = 0
			region->in_region_conflict_sum += conflict_cnt;
	}
	return conflict_cnt;
}

void __wait_last_reset_done(struct remote_context *rc)
{
	SYNCPRINTK2("L:%d R:%d\n",
			atomic_read(&rc->per_barrier_reset_done), rc->remote_done_cnt);
	/* wait real clean to catch up hand shake, If I don't proceed, handshke will not proceed, rc->remote_done_cnt should not proceed as well. */
	//printk("remote mg start - 1\n");
	while (atomic_read(&rc->per_barrier_reset_done) != rc->remote_done_cnt) {
		CPU_RELAX; //origin and only
		smp_rmb();	// potential BUG(); TODO testing important
	}
	/* for race condition, prevent from overwritten */ /* This may be a straggler case PERF */ /* no sleep may casue a 100% kernel spining (bus)*/
}

void __wait_local_list_ready(struct remote_context *rc, int wr_cnt)
{
	BARRMPRINTK("prepare to find conflict at remote req->wr_cnt %d (wait rc->ready)\n", wr_cnt);
	/* RC: make sure global list is ready - wait on cache line */
	//printk("remote mg start - 2\n");
	while(!rc->ready) {
		CPU_RELAX;
		smp_rmb(); // potential BUG(); TODO testing important
	}
	/* TODO bug spinging on bare now */
	BARRMPRINTK("find conflict at remote req->wr_cnt %d (barrier)\n", wr_cnt);
}

/* -> [Remote](multiple) diffs + merg_res => order matters */
static void process_page_merge_request(struct work_struct *work)
{
	START_KMSG_WORK(page_merge_request_t, req, work);
    //page_merge_response_t *res = pcn_kmsg_get(sizeof(*res)); // test
    page_merge_response_t *res = kmalloc(sizeof(*res), GFP_KERNEL);
	struct task_struct *tsk = __get_task_struct(req->remote_pid);
	struct remote_context *rc;
	int conflict_cnt;
	int wr_cnt = req->wr_cnt;
	unsigned long *wr_addrs = req->addrs;
	BUG_ON(!tsk);
	rc = get_task_remote(tsk);
	BUG_ON(!res|| !rc);

	/* put more handshake info to detect skew cases */
	//req->begin =
	//current->begin_m_cnt
	//current->begin_cnt
	//req->fence = current->tso_fence_cnt;
	rc->remote_fence = req->fence;
	//BUG_ON(rc->fence != current->tso_fence_cnt); // too early
	//req->end =
	//mb %lu b %lu f %lu

	// testing !!!! TODO this should be done in another side? or later
	if (req->iter == req->total_iter) {
		rc->remote_merge_id = req->merge_id; /* handshake remote status */
		smp_wmb();
	}

	/* Barriers */
	__wait_last_reset_done(rc); /* not going to work - instead use begin */
	__wait_local_list_ready(rc, wr_cnt);

	conflict_cnt =
		__find_collision_btw_nodes_at_remote(req, rc, tsk, wr_cnt, wr_addrs);
	//printk("remote mg done\n");
	BARRMPRINTK("\t\t{{{ conflict_cnt at remote %d/%d }}}\n",
										conflict_cnt, wr_cnt);
	//printk("\t\t{{{ conflict_cnt at remote %d/%d }}}\n",
	//									conflict_cnt, wr_cnt);

	res->origin_pid = req->origin_pid;
    res->origin_ws = req->origin_ws;
    //res->remote_pid = req->remote_pid;

	/* not only 1 page man... 32k-4 8-1 = 7diffs......*/
//	res->merge_id = iter_num;

	res->wr_cnt = req->wr_cnt;
	res->iter = req->iter;
	res->total_iter = req->total_iter;
	res->conflict_cnt = conflict_cnt; /* make sure origin will wait until all done. origin will calculate and get this same number */

	WSYPRINTK("\t\t<- MERGE request [%d/%d] [%s] lr %d/%d\n",
					req->iter, req->total_iter,
					req->iter == req->total_iter ? "*" : "",
					rc->local_merge_id,
					rc->remote_merge_id);

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
	rc->remote_type = req->remote_region_type;

	rc->remote_done_cnt++; /* remote barrier */
	smp_wmb();
	WSYPRINTK("\t  <- handshake lr(%d/%d)\n",
					rc->local_done_cnt, rc->remote_done_cnt);

	__put_task_remote(rc);
    put_task_struct(tsk);
	pcn_kmsg_done(req);
	return 0;
}


// * 1. fixup (async single now)
/* -> back[Local](main)(many_scatters) */
//static int handle_page_merge_response(struct pcn_kmsg_message *msg)
static void process_page_merge_response(struct work_struct *work)
{
    //page_merge_response_t *res = (page_merge_response_t *)msg;
    START_KMSG_WORK(page_merge_response_t, res, work);
    struct wait_station *ws = wait_station(res->origin_ws);
	struct task_struct *tsk = __get_task_struct(res->origin_pid);
	struct remote_context *rc = get_task_remote(tsk);
	int remote_req_merge_left = atomic_dec_return(&rc->scatter_pendings);
	int i;
	BUG_ON(!rc || !tsk);

	/* merges are all done in diff_apply per page */

	//res->wr_cnt = req->wr_cnt;
	//res->iter = req->iter;
	//res->total_iter = req->total_iter;
	/* TODO get remote conflict_cnt back. and check all diff done? */
	/* TODO get remote conflict_cnt back. and check all diff done? */
	/* TODO get remote conflict_cnt back. and check all diff done? */
	for (i = 0; i < res->conflict_cnt; i++)
		atomic_inc(&rc->doing_diff_cnt);

//done:
	/* last MERGE response */
	if (!remote_req_merge_left) { /* done */
		WSYPRINTK("\t\t<- MERGE response [*] ws %p rmid %d\n",
									ws, rc->remote_merge_id);
		// must follow order - this makes this func WQ
		if (my_nid == NOCOPY_NODE)
			while (atomic_read(&rc->doing_diff_cnt)) // BUG still corner case //
				CPU_RELAX;

		complete(&ws->pendings); /* wake up leader thread */
	} else if (remote_req_merge_left > 0) {
		WSYPRINTK("\t\t<- MERGE response [ ]\n");
	} else {
		BUG();
	}

	__put_task_remote(rc);
    put_task_struct(tsk);
    END_KMSG_WORK(res);
    //pcn_kmsg_done(res);
    //return 0;
}

/* (serial) - clean per barrier data in rc */
static void __per_barrier_reset(struct remote_context *rc)
{
	rc->ready = false; /* sync */
	rc->remote_done = false;
	rc->remote_type = RCSI_UNKNOW;
	atomic_set(&rc->diffs, 0); /* race condition */ /* async + multiple */ /* 0 0 is a true false case */
	atomic_set(&rc->req_diffs, 0); /* 0 0 is a true false case */

	__tso_wr_clean_all(rc);
	atomic_inc(&rc->per_barrier_reset_done); /* >0: done , 0: !done*/
	smp_wmb();
}

/*
 * put rc and pirnk outsite !!!!!!!!!!1
 * be mor general
 */
static bool __popcorn_start_begin_barrier(struct remote_context *rc, int id)
{
	bool leader = false;
	int left_t = atomic_dec_return(&rc->barrier_end);
	BUG_ON(rc->threads_cnt > MAX_THREADS);
	if (left_t == 0) {
		leader = true;
		BARRPRINTK("=== +++[*%d] BARRIER %5s! line %d mb %lu b %lu f %lu t %d "
				"Hand Shake done. ===\n",
				current->pid, "BEGIN", id, current->begin_m_cnt,
				current->begin_cnt, current->tso_fence_cnt, rc->threads_cnt);
	} else if (left_t < 0) {
		BUG();
	}

	return leader;
}

void __wait_end_followers(struct remote_context *rc)
{
	/* Race Condition !!!!! wait all are in the wq */
	while (atomic_read(&rc->pendings_end) != rc->threads_cnt - 1)
		CPU_RELAX; // seems important to yield the atomic op bus in VM
}

void __wait_remote_done_handshake(struct remote_context *rc)
{
	/* remote also all done (handshake) */
	SYNCPRINTK2("\t- wait lr(%d/%d) diffs %d req_diffs %d remote_done %d\n",
				rc->local_done_cnt, rc->remote_done_cnt,
				atomic_read(&rc->diffs), atomic_read(&rc->req_diffs),
													rc->remote_done);
	while (rc->local_done_cnt > rc->remote_done_cnt) { /* wait for remote */
		CPU_RELAX;
		smp_rmb(); // potential BUG(); TODO testing important
	}
	/* at this moment - remote might have sent to me MERGE req */
	/* race condition rc-> diffs is 0 but reset by later -1 */
	WSYPRINTK("\t- *pass handshake lr(%d/%d)\n",
				rc->local_done_cnt, rc->remote_done_cnt);

#if STRONG_CHECK_SANITY
	// gurdian
	if (rc->local_done_cnt != rc->remote_done_cnt) {
		WSYPRINTK(KERN_ERR "lf: %lu rf: %d\n",
				current->tso_fence_cnt, rc->remote_fence);
		//BUG_ON(rc->remote_fence != current->tso_fence_cnt); /* gurdian false-true */
	}
#endif
}

/* According to local + remote result, make desicion */
void __smart_region_handshake(struct remote_context *rc, struct omp_region *region)
{
	if (region->type != rc->remote_type) { /* different region prediction */
		if (region->type & RCSI_VAIN)  //  optimize code style
			if (!(rc->remote_type & RCSI_VAIN)) {
				region->type &= ~RCSI_VAIN; /* roll back - conservative */
#if SMART_REGION_PERF_DBG
//				printk("[0x%lx] %d ROLL BACK to VAIN 0x%lx %d\n",
//										region->id, rc->inv_cnt,
//										region->type, region->cnt);
#endif
			}
	}
}

static void __popcorn_end_end_barrier(struct remote_context *rc, int id, bool leader, struct omp_region *region)
{
	if (leader) {
		BARRPRINTK("=== ---[*%d] BARRIER %5s! line %d mb %lu b %lu f %lu t %d "
					"Hand Shake done. ===\n\n",
					current->pid, "END", id, current->begin_m_cnt,
					current->begin_cnt, current->tso_fence_cnt, rc->threads_cnt);

		//if(!current->at_remote){PFVPRINTK("= *BARRIER wait =\n");}
		__wait_end_followers(rc);
		__wait_remote_done_handshake(rc);
#if SMART_REGION
		__smart_region_handshake(rc, region);
#endif

		__per_barrier_reset(rc); /* CLEAN before waking up followers */
		atomic_inc(&rc->pendings_end);
		atomic_set(&rc->barrier_end, rc->threads_cnt);
		wake_up_all(&rc->waits_end);
		atomic_dec(&rc->pendings_end);

		//printk("= done *BARRIER wait =\n");
		BARRPRINTK("=== ---[*%d] BARRIER %5s! line %d mb %lu b %lu f %lu t %d "
				"Hand Shake done. ===\n\n",
				current->pid, "END*", id, current->begin_m_cnt,
				current->begin_cnt, current->tso_fence_cnt, rc->threads_cnt);
	} else {
		DEFINE_WAIT(wait);
		//SYNCPRINTK("[ ][%d] left_t %d\n", current->pid, left_t);
		prepare_to_wait_exclusive(&rc->waits_end, &wait, TASK_UNINTERRUPTIBLE);
        atomic_inc(&rc->pendings_end);
		io_schedule();
		finish_wait(&rc->waits_end, &wait);
		atomic_dec(&rc->pendings_end);
	}

#if CONSERVATIVE
	/* redudnat barrier - test - conservative */
	while (atomic_read(&rc->pendings_end))
		CPU_RELAX;
	BUG_ON(atomic_read(&rc->pendings_end) < 0);
#endif
}

void collect_tso_wr(struct task_struct *tsk)
{
#if STATIS // put for all variables
	struct remote_context *rc = get_task_remote(tsk);
	printk("[%d]: %s exit (rc) sys_rw_cnt %lu "
						"sys_ww_cnt %d (remote side %lu)  "
						"sys_inv_cnt %lu "
						"sys_local_conflict_cnt %lu "
						"violation_begin %d violation_end %d "
						"curr->smart_skip_cnt %d "
						"pf_cnt %d\n",
						tsk->pid, tsk->comm,
						rc->sys_rw_cnt,
						atomic_read(&rc->sys_ww_cnt),
						rc->remote_sys_ww_cnt,
						rc->sys_rw_cnt - rc->remote_sys_ww_cnt,
		//rc->sys_rw_cnt - atomic_read(&rc->sys_ww_cnt) - rc->remote_sys_ww_cnt,
						rc->sys_local_conflict_cnt,
						violation_begin, violation_end, current->smart_skip_cnt,
						atomic_read(&rc->pf_succ_cnt));
						/* ww_cnt is a bad name */
	__put_task_remote(rc);
#endif
}


#include <asm/uaccess.h>
static void __parse_save_region_info(struct omp_region *region, void __user * file)
{
#if SMART_REGION_DBG
	/* delaying is fine */
	if (region->name[0] == '\0') {
		if (file) {
			int ofs = 0;
			while (access_ok(VERIFY_READ, file + ofs, 1)) {
				if (!((char*)file + ofs)) break;
				BUG_ON(copy_from_user(region->name + ofs, file + ofs , 1));
				if (*(region->name + ofs) == ';') break;
				ofs++;
			}
			memset(region->name + ofs, '\0' , 1);
			//printk("region->name %s\n", region->name);
		}
	}

	if (!region->line && id)
		region->line = id;
#endif
}

static int __popcorn_tso_begin(int id, void __user * file, unsigned long omp_hash, int a, void __user * b)
{
	struct omp_region *region;
	struct remote_context *rc = current->mm->remote;
    current->tso_region_id = omp_hash; /* for detecting as a skipped region */
	//if (current->tso_region || current->tso_wr_cnt || current->tso_wx_cnt) {
	if (current->tso_region) {
		WARN_ON_ONCE("BUG \"tso_begin_manual\" order violation");
		if (!print) {
#if REENTRY_BEGIN_DISABLE
			print = true;
#endif
#if !SMART_REGION && !SMART_REGION_DBG
			PCNPRINTK_ERR("[%d] BUG \"tso_begin_manual\" order violation "
					"region (%s) tso_wr %llu tso_wx %llu line %d omp_hash 0x%lx\n",
					current->pid, current->tso_region?"O":"X",
					current->tso_wr_cnt, current->tso_wx_cnt, id, omp_hash);
#endif
		}
		violation_begin++;
	}

	region = __omp_region_hash(rc, omp_hash);
	if(region) {
		bool leader;
		__region_total_cnt_inc(region);
#if SMART_REGION
		if (region->type & RCSI_VAIN) { /* smart wr */
			SMRPRINTK("[%d] %s(): omp_hash 0x%lx bm_cnt %lu no benefit **SKIP**\n",
						current->pid, __func__, omp_hash, current->begin_m_cnt++);
			__region_skip_cnt_inc(region);
			return 0;
		}
#endif
		__parse_save_region_info(region, file);

#if GOD_VIEW
		/* prefetch pages */
		leader = __start_begin_barrier();
		if (leader) {
			struct sys_omp_region *sys_region =
				__god_view_prefetch(rc, omp_hash, region->cnt);
			if (!sys_region) {
				write_lock(&sys_region_hash_lock);
				__ondemand_si_region_create(omp_hash, region->cnt);
				write_unlock(&sys_region_hash_lock);
			}
		}
		__end_begin_barrier(leader);

		current->omp_hash = omp_hash;
		current->region_cnt = region->cnt;
#endif
	}

	/* TODO: THINK HARD: Doing readfault related works here
		is costly since it has to stop all threads */

	current->tso_region = true;
	RGNPRINTK("[%d] %s(): omp_hash 0x%lx cnt %lu\n", current->pid,
				__func__, omp_hash, current->begin_m_cnt++);
	return 0;
}

void __wait_diffs_done(struct remote_context *rc)
{
	/* sync: 1. wait diffs 2. wait merge msgs */
	//if (current->tso_region){printk("wait diffs\n");}
	//if(!current->at_remote){printk("wait diffs\n");}
	//PFVPRINTK("wait all diffs done (origin only)\n");
	if (my_nid == NOCOPY_NODE) {
		/* local calculated diff cnt VS remote calculated diff cnt that has been done at origin) */
		while (atomic_read(&rc->diffs) != atomic_read(&rc->req_diffs)) {
			CPU_RELAX;
		}
		WSYPRINTK("diffs sync pass: l calc %d r calc %d (origin only) lr(%d/%d)\n",
					atomic_read(&rc->diffs), atomic_read(&rc->req_diffs),
					rc->local_merge_id, rc->remote_merge_id);
	}
	//printk("done wait diffs\n");
	//if (current->tso_region){printk("done wait diffs\n");}
}

/* Prevent from double handshake
 * (As long as in leader & before END_B) */
void __wait_merge_msgs(struct remote_context *rc)
{
	/* BUG() - if spining here, may mean never recv the merge req */
	SYNCPRINTK2("mg: wait lr(%d/%d)\n",
				rc->local_merge_id, rc->remote_merge_id);
	//if(!current->at_remote){printk("mg: wait\n");}
	while(rc->local_merge_id > rc->remote_merge_id) { //BUG: remote_merge_id is updated in the first merg_req() just wait one
		CPU_RELAX;
		smp_rmb(); // potential BUG(); TODO testing important
	}

	/* going to handshake */
	//printk("mg: done wait\n");
	SYNCPRINTK2("mg: pass lr(%d/%d)\n",
				rc->local_merge_id, rc->remote_merge_id);

	rc->local_done_cnt++;
	smp_wmb();
	WSYPRINTK("\t-> handshake lr(%d/%d) diffs %d req_diffs %d "
				"remote_done %d\n", rc->local_done_cnt, rc->remote_done_cnt,
					atomic_read(&rc->diffs), atomic_read(&rc->req_diffs),
					rc->remote_done);
}

bool is_skipped_region(struct task_struct *tsk)
{
	return !tsk->tso_region && tsk->tso_region_id;
}

/* Don't trust this omp_hash. should consistent with begin */
static int __popcorn_tso_fence(int id, void __user * file, unsigned long omp_hash, int a, void __user * b)
{
	bool leader;
	struct omp_region *region;
	struct remote_context *rc = current->mm->remote;
#if SMART_REGION_DBG
	if(is_skipped_region(current))
		__try_flip_region_type(rc);
#endif
	if (!current->tso_region) { /* open to detect errors */
#if !SMART_REGION && !SMART_REGION_DBG
// uncomment it
//		PCNPRINTK_ERR("[%d] BUG fence in a not-tso region "
//						"tso_region_id %lu line %d omp_hash 0x%lx - IGNORE\n",
//						current->pid, current->tso_region_id, id, omp_hash);
//								/* warnning - this omp has is not begin */
#endif
		current->tso_region_id = 0; // the rest of region -> X
		goto out;
	}

#if POPCORN_TSO_BARRIER
	leader = __popcorn_start_begin_barrier(rc, id);
	if (leader) {
		//if (current->tso_region){printk("MERGE wait\n");}
		__locally_find_conflictions(TO_THE_OTHER_NID(), rc);
		//if (current->tso_region){printk("MERGE passed\n");}
#if GOD_VIEW
		// dbg - before region_cnt++ just in case
		__dbg_si_addrs(current->omp_hash, current->region_cnt);
#endif

		region = __analyze_region(rc);
		region->cnt++;

		__wait_diffs_done(rc); /* till local == remote calculated # done local  */
		__wait_merge_msgs(rc);

		/* conservertive order - after wait_merge() */
		__sned_handshake(rc, TO_THE_OTHER_NID(), region);
	}
	__popcorn_end_end_barrier(rc, id, leader, region);
#endif

#if 0
	// not clean addrs[]
#endif
	current->tso_wr_cnt = 0;
	current->tso_wx_cnt = 0;
	current->tso_region_id = 0; /* s -> f xxxx e */ // the rest of  region -> X
	current->tso_fence_cnt++;
out:
	current->read_cnt = 0;
	current->writenopg_cnt = 0;
	return 0;
}

static int __popcorn_tso_end(int id, void __user * file, unsigned long omp_hash, int a, void __user * b)
{
	RGNPRINTK("[%d] %s(): id %d omp_hash 0x%lx -> implicit fence\n", current->pid, __func__, id, omp_hash);
#if !SMART_REGION
	if (!current->tso_region || !current->tso_region_id) {
#if IGNORE_WARN_ON_ONCE
		WARN_ON_ONCE("BUG \"tso_end\" order violation");
		if (!print_end) {
#if REENTRY_BEGIN_DISABLE
			print_end = true;
#endif

#if !SMART_REGION && !SMART_REGION_DBG
			PCNPRINTK_ERR("[%d] BUG \"tso_end\" order violation "
						"region (%s) tso_region_id %lu line %d omp_hash 0x%lx\n",
									current->pid, current->tso_region?"O":"X",
									current->tso_region_id, id, omp_hash);
#endif
		}
#endif
		violation_end++;
	}
#endif

	if (!current->tso_region) return 0;
    trace_tso(my_nid, current->pid, id, 'e');
	__popcorn_tso_fence(id, file, omp_hash, a, b);
	current->tso_region = false;
	//current->tso_region_id = 0;
	current->tso_region_cnt++;
	return 0;
}


/*
 * Syscalls
 */
#ifdef CONFIG_POPCORN
SYSCALL_DEFINE5(popcorn_tso_begin, int, id, void __user *, file, unsigned long, omp_hash, int, a, void __user *, b)
{
	return __popcorn_tso_begin(id, file, omp_hash, a, b);
}

SYSCALL_DEFINE5(popcorn_tso_fence, int, id, void __user *, file, unsigned long, omp_hash, int, a, void __user *, b)
{
	RGNPRINTK("[%d] %s(): id %d omp_hash 0x%lx\n", current->pid, __func__, id, omp_hash);
    trace_tso(my_nid, current->pid, id, 'f');
	return __popcorn_tso_fence(id, file, omp_hash, id, b);
}

SYSCALL_DEFINE5(popcorn_tso_end, int, id, void __user *, file, unsigned long, omp_hash, int, a, void __user *, b)
{
	return __popcorn_tso_end(id, file, omp_hash, a, b);
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
	return __popcorn_tso_begin(id, file, omp_hash, a, b);
}

SYSCALL_DEFINE5(popcorn_tso_fence_manual, int, id, void __user *, file, unsigned long, omp_hash, int, a, void __user *, b)
{
	RGNPRINTK("[%d] %s():\n", current->pid, __func__);
	return __popcorn_tso_fence(id, file, omp_hash, a, b);
}

SYSCALL_DEFINE5(popcorn_tso_end_manual, int, id, void __user *, file, unsigned long, omp_hash, int, a, void __user *, b)
{
	return __popcorn_tso_end(id, file, omp_hash, a, b);
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

DEFINE_KMSG_WQ_HANDLER(page_merge_response);
DEFINE_KMSG_WQ_HANDLER(page_merge_request); // sleep
//DEFINE_KMSG_WQ_HANDLER(remote_prefetch_request);
//DEFINE_KMSG_WQ_HANDLER(remote_prefetch_response);
DEFINE_KMSG_ORDERED_WQ_HANDLER(remote_prefetch_request); // fast recv full
DEFINE_KMSG_ORDERED_WQ_HANDLER(remote_prefetch_response);
int __init popcorn_sync_init(void)
{
	//__rcsi_hash_test();
	__rcsi_mem_alloc();

	REGISTER_KMSG_WQ_HANDLER(
            PCN_KMSG_TYPE_PAGE_MERGE_REQUEST, page_merge_request);
    REGISTER_KMSG_WQ_HANDLER( // sleep
    //REGISTER_KMSG_HANDLER(	/* main scatters */
            PCN_KMSG_TYPE_PAGE_MERGE_RESPONSE, page_merge_response); // order wq
    REGISTER_KMSG_HANDLER(	/* diff - one way - not sort */
            PCN_KMSG_TYPE_PAGE_DIFF_APPLY_REQUEST, page_apply_diff_request); // order wq

	/* IMM response */
    REGISTER_KMSG_HANDLER(	/* my side all done signal - one way - only one */
            PCN_KMSG_TYPE_PAGE_MERGE_DONE_REQUEST, page_diff_all_done_request);

    //REGISTER_KMSG_HANDLER(	/* msg lost somehow */ /* main scatters */
    REGISTER_KMSG_WQ_HANDLER( // sleep
        PCN_KMSG_TYPE_REMOTE_PREFETCH_REQUEST, remote_prefetch_request);
    //REGISTER_KMSG_HANDLER(	/* msg lost somehow */ /* main scatters */
    REGISTER_KMSG_WQ_HANDLER( // fixup /* pg fixup may sleep */ // loose
        PCN_KMSG_TYPE_REMOTE_PREFETCH_RESPONSE, remote_prefetch_response);

#if 0
	printk("hope to see two goods\n");
#define ONE 1
	if(!ONE)
		printk("bad\n");
	else
		printk("good testing? !ONE %d \n", !ONE);

#define ZZ 0
	if(!ZZ)
		printk("good 100%% !ZZ %d\n", !ZZ);
	else
		printk("bad\n");
#endif

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
	printk("(1 << MAX_ORDER) * PAGE_SIZE %lu\n", (1 << MAX_ORDER) * PAGE_SIZE);
	printk("struct sys_omp_region = %lu\n", sizeof(struct sys_omp_region));
	printk("struct sys_omp_region = %lu\n", sizeof(struct sys_omp_region));
	printk("struct sys_omp_region = %lu\n", sizeof(struct sys_omp_region));
	printk("%lu pgs\n", sizeof(struct sys_omp_region)/PAGE_SIZE);
	printk("%lu pgs\n", sizeof(struct sys_omp_region)/PAGE_SIZE);
	printk("%lu pgs\n", sizeof(struct sys_omp_region)/PAGE_SIZE);
	if (sizeof(struct sys_omp_region) > (1 << MAX_ORDER) * PAGE_SIZE) {
		printk("(1 << MAX_ORDER) * PAGE_SIZE %lu > struct sys_omp_region = %lu\n",
				 (1 << MAX_ORDER) * PAGE_SIZE, sizeof(struct sys_omp_region));
	}
	//BUG_ON(sizeof(struct sys_omp_region) > 1 << MAX_ORDER);

	return 0;
}
