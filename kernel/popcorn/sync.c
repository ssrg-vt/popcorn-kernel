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

#define RCSI_IRQ_CPU 1
unsigned long handle_mw_reqest_cpu[MAX_THREADS];
unsigned long handle_pf_reqest_cpu[MAX_THREADS];

/************** working zone *************/
#define SMART_REGION_PERF_DBG 0

#define SMART_REGION_DBG 0 /* to debug smart region working behaviour */

#define CURRENT_DEBUG 0
#if CURRENT_DEBUG
#define CURRPRINTK(...) printk(KERN_INFO __VA_ARGS__)
#else
#define CURRPRINTK(...)
#endif
/************** working zone *************/

/*** cpu oriented req ***/
/* 1: cpu oriented 0: batch oriented */
#if VM_TESTING
#define MW_CONSIDER_CPU 0
#define PF_CONSIDER_CPU 0
#else
#define MW_CONSIDER_CPU 1
#define PF_CONSIDER_CPU 1 // TODO: implemente
#endif

/*** perf - time statis ***/
#define PF_TIME 1
#define MW_TIME 1
#define TSO_TIME 1 // aka MW log time


/*** minor features ****/
#define REENTRY_BEGIN_DISABLE 1 // 0: repeat show 1: once


/*** perf ***/
#define SKIP_MEM_CLEAN 1 // testing
#define CONSERVATIVE 0 // this is BETA 1:safe
#define PERF_FULL_BULL_WARN 1 // only for prink statis still works and show in the end !!!


/*** statis for making pf decisions  ***/
#define DECISION_PREFETCH_STATIS 0


/** warn **/
#define IGNORE_WARN_ON_ONCE 1


#define CPU_RELAX io_schedule()
//#define CPU_RELAX

#define PREFETCH_WRITE 0 /* NOT READY */


#define PREFETCH_VERBOSE_DBG 0
#if PREFETCH_VERBOSE_DBG
#define PFVPRINTK(...) printk(KERN_INFO __VA_ARGS__)
#else
#define PFVPRINTK(...)
#endif

//#define WW_SYNC_VERBOSE_DBG 1
#define WW_SYNC_VERBOSE_DBG 0
#if WW_SYNC_VERBOSE_DBG
#define WSYPRINTK(...) printk(KERN_INFO __VA_ARGS__)
#else
#define WSYPRINTK(...)
#endif


//#define MWCPU_DEBUG 1
#define MWCPU_DEBUG 0
#if MWCPU_DEBUG
#define MWCPUPRINTK(...) printk(KERN_INFO __VA_ARGS__)
#else
#define MWCPUPRINTK(...)
#endif


#define SMART_REGION_DBG_DBG 0
#if SMART_REGION_DBG_DBG
#define SMRPRINTK(...) printk(KERN_INFO __VA_ARGS__)
#else
#define SMRPRINTK(...)
#endif

//#define BARRIER_INFO 1
#define BARRIER_INFO 0
#if BARRIER_INFO
#define BARRPRINTK(...) printk(KERN_INFO __VA_ARGS__)
#else
#define BARRPRINTK(...)
#endif

//#define BARRIER_INFO_MORE 1
#define BARRIER_INFO_MORE 0
#if BARRIER_INFO_MORE
#define BARRMPRINTK(...) printk(KERN_INFO __VA_ARGS__)
#else
#define BARRMPRINTK(...)
#endif

//#define SYNC_DEBUG_THIS 1
#define SYNC_DEBUG_THIS 0
#if SYNC_DEBUG_THIS
#define SYNCPRINTK2(...) printk(KERN_INFO __VA_ARGS__)
#else
#define SYNCPRINTK2(...)
#endif

#define DIFF_APPLY 0
//#define DIFF_APPLY 1
#if DIFF_APPLY
#define DIFFPRINTK(...) printk(KERN_INFO __VA_ARGS__)
#else
#define DIFFPRINTK(...)
#endif

//#define BUF_DBG 1
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

/* !perf */
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
#define SYS_REGION_HASH_BITS 10 /* TODO TODO TODO try larger */
DEFINE_HASHTABLE(sys_region_hash, SYS_REGION_HASH_BITS);
DEFINE_RWLOCK(sys_region_hash_lock);
bool is_god = true;


/* Violation section detecting */
static bool print = false;
#if !SMART_REGION
static bool print_end = false;
#endif

static int violation_begin = 0;
#if !SMART_REGION
static int violation_end = 0;
#endif

/* statis time */
#if PF_TIME
unsigned long pf_time = 0;
unsigned long pf_malloc_time = 0;

unsigned long pf_time_wait_send = 0;
unsigned long pf_time_wait_fixup = 0;

atomic64_t pf_log_time = ATOMIC64_INIT(0);

unsigned long pf_pgs_peak = 0;
unsigned long pf_pgs_total = 0;
unsigned long pf_pgs_cnt = 0;

unsigned long pf_send_peak = 0;
unsigned long pf_send_total = 0;
unsigned long pf_send_cnt = 0;

unsigned long pf_threshold_skip_pgs = 0;
#endif

#if MW_TIME
unsigned long mw_time = 0;
unsigned long mw_time_find_conf = 0;
unsigned long mw_time_wait_diffs = 0;
unsigned long mw_time_wait_merges = 0;
unsigned long mw_time_wait_end = 0;

unsigned long mw_time_find_conf_wait_scatters = 0;
unsigned long mw_time_find_conf_wait_sends = 0;
unsigned long mw_time_find_conf_wait_res = 0;
unsigned long mw_time_find_conf_wait_applys = 0;

unsigned long mw_time_wait_remotehs = 0;

unsigned long mw_inv_peak = 0;
unsigned long mw_inv_total = 0;
unsigned long mw_inv_cnt = 0;

unsigned long mw_send_peak = 0;
unsigned long mw_send_total = 0;
unsigned long mw_send_cnt = 0;
#endif

#if TSO_TIME
atomic64_t mw_log_time = ATOMIC64_INIT(0);
#endif


/*** debugging ***/
#define KMPC_BARRIER 10
#define KMPC_CANCEL_BARRIER 11

#define KMPC_REDUCE 20
#define KMPC_END_REDUCE 21
#define KMPC_REDUCE_NOWAIT 22
#define KMPC_DISPATCH_FINI 23

#define HHB 30 //hierarchy_hybrid_barrier 30
#define HHCB 31 //hierarchy_hybrid_cancel_barrier 31
#define HHBF 32 //hierarchy_hybrid_barrier_final 32

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
	char name[80];
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

	int read_cnt;
	int writenopg_cnt;
	unsigned long read_addrs[MAX_READ_BUFFERS * MAX_THREADS / 5]; // hardcode TODO
	unsigned long writenopg_addrs[MAX_WRITE_NOPAGE_BUFFERS * MAX_THREADS / 5]; // hardcode TODO

	/* two hash tables for addrs */
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

void mw_results(struct remote_context *rc)
{
#if MW_TIME
#if CONFIG_X86_64
	long int divider = X86_THREADS;
#else
	long int divider = ARM_THREADS;
#endif
	printk("======================\n");
	printk("=== MW parameters ===\n");
	printk("======================\n");
	printk("- MW (%s)\n", MW ? "O" : "X");
	printk("- MW_CONSIDER_CPU (%s)\n", MW_CONSIDER_CPU ? "O" : "X");
	printk("- MAX_WRITE_INV_BUFFERS %ld\n", MAX_WRITE_INV_BUFFERS);

	//MW SMART OMP REGION
	printk("- SMART REGION (%s)\n", SMART_REGION ? "O" : "X");
	printk("- VAIN_THRESHOLD %d (cpu other side)\n", VAIN_THRESHOLD);
	printk("- VAIN_REGION_REPEAT_THRESHOLD %d\n", VAIN_REGION_REPEAT_THRESHOLD);
	//printk("BENEFIT_REGION_REPEAT_THRESHOLD (%s)\n", BENEFIT_REGION_REPEAT_THRESHOLD ? "O" : "X");
	//printk(" (%s)\n",  ? "O" : "X");

	printk("mw: inv addrs\n");

	printk("======================\n");
	printk("=== MW results ===\n");
	printk("======================\n");
	// TODO other times like mw memcpy
	printk("mw_time [[%lu]]* s =>\n", mw_time / NANOSECOND);
	printk("\tlocal_find_conf [[%lu]]* wait_diffs %lu wait_merg %lu wait_end %lu "
										"& wait_remotehs %lu s\n",
										mw_time_find_conf / NANOSECOND,
										mw_time_wait_diffs / NANOSECOND,
										mw_time_wait_merges / NANOSECOND,
										mw_time_wait_end / NANOSECOND,
										mw_time_wait_remotehs / NANOSECOND);
	printk("\t\twait_scatters %lu wait_sends %lu "
						"wait_res %lu wait_applys %lu\n",
						mw_time_find_conf_wait_scatters / NANOSECOND,
						mw_time_find_conf_wait_sends / NANOSECOND,
						mw_time_find_conf_wait_res / NANOSECOND,
						mw_time_find_conf_wait_applys / NANOSECOND);
	printk("\t\t\tmw_inv peak %lu avg %lu\n",
				mw_inv_peak, mw_inv_cnt ? mw_inv_total / mw_inv_cnt : 0);
	printk("\t\t\tmw req send peak %lu avg %lu\n",
				mw_send_peak, mw_send_cnt ? mw_send_total / mw_send_cnt : 0);
	printk("mw_log_time %ld ms\n",
				atomic64_read(&mw_log_time) / MICROSECOND / divider);
	
	{	int i;
		printk("handle_mw_reqest_cpu: ");
		for (i = 0; i < MAX_THREADS; i++) {
			if (handle_mw_reqest_cpu[i])
				printk("[%d] %lu ", i, handle_mw_reqest_cpu[i]);
		}
		printk("\n");
	}
	printk("TODO: WW merge one by one now -> more \n");
#endif
}

void pf_results(struct remote_context *rc)
{
#if CONFIG_X86_64
	long int divider = X86_THREADS;
#else
	long int divider = ARM_THREADS;
#endif
#if PF_TIME
	printk("======================\n");
	printk("=== PF parameters ===\n");
	printk("======================\n");
	//printk("GOD_VIEW (%s)\n", GOD_VIEW ? "O" : "X");
	//printk("PREFETCH (%s)\n", PREFETCH ? "O" : "X");
	//printk(" (%s)\n",  ? "O" : "X");
	printk("- PREFETCH_THRESHOLD %lu per msg\n", PREFETCH_THRESHOLD);
	printk("- MAX_PF_MSG %d\n", MAX_PF_MSG);
	printk("- PF_CONSIDER_CPU (%s) TODO: implemente\n",
							PF_CONSIDER_CPU  ? "O" : "X");

	printk("======================\n");
	printk("=== PF results ===\n");
	printk("======================\n");
	printk("real prefetch cnt pf_cnt [[%d]]!!\n",
						atomic_read(&rc->pf_succ_cnt));

	printk("pf_time [[%lu]]* s =>\n", pf_time / NANOSECOND);
	printk("\tpf_wait_send %lu pf_wait_fixup [[%lu]] s\n",
						pf_time_wait_send / NANOSECOND,
						pf_time_wait_fixup / NANOSECOND);
	printk("\t\t\tpf_pgs peak [[%lu]] avg [[%lu]]\n",
			pf_pgs_peak, pf_pgs_cnt ? pf_pgs_total / pf_pgs_cnt : 0);
	printk("\t\t\tpf_send peak [[%lu]] avg [[%lu]]\n",
			pf_send_peak, pf_send_cnt ? pf_send_total / pf_send_cnt : 0);

	printk("\t\tpf_malloc_time %lu s\n", pf_malloc_time / NANOSECOND);
	printk("pf_log_time %ld ms\n",
				atomic64_read(&pf_log_time) / MICROSECOND / divider);

	/* skip */
	printk("\t\t\tpf_threshold_skip_pgs %lu (pgs)\n", pf_threshold_skip_pgs);
	{	int i;
		printk("handle_pf_reqest_cpu: ");
		for (i = 0; i < MAX_THREADS; i++) {
			if (handle_pf_reqest_cpu[i])
				printk("[%d] %lu ", i, handle_pf_reqest_cpu[i]);
		}
		printk("\n");
	}
#endif
}

/*by write*/
void rcsi_time_stat(struct seq_file *seq, void *v)
{
    if (seq) {
		// hard to get rc here
		//mw_results(rc);
		//pf_results(rc);
	} else {
#if MW_TIME
		atomic64_set(&mw_log_time, 0);
		mw_time = 0;
		mw_time_find_conf = 0;
		mw_time_wait_diffs = 0;
		mw_time_wait_merges = 0;
		mw_time_wait_end = 0;
		mw_time_wait_remotehs = 0;

		mw_time_find_conf_wait_scatters = 0;
		mw_time_find_conf_wait_sends = 0;
		mw_time_find_conf_wait_res = 0;
		mw_time_find_conf_wait_applys = 0;

		mw_inv_peak = 0;
		mw_inv_total = 0;
		mw_inv_cnt = 0;

		mw_send_peak = 0;
		mw_send_total = 0;
		mw_send_cnt = 0;
		printk("clean mw_log_time pf_threshold_skip_pgs\n");
#endif

#if PF_TIME
		pf_time = 0;
		pf_malloc_time = 0;

		pf_time_wait_send = 0;
		pf_time_wait_fixup = 0;

		atomic64_set(&pf_log_time, 0);

		pf_pgs_peak = 0;
		pf_pgs_total = 0;
		pf_pgs_cnt = 0;

		pf_send_peak = 0;
		pf_send_total = 0;
		pf_send_cnt = 0;

		pf_threshold_skip_pgs = 0;
		printk("clean pf_log_time\n");
#endif

#if RCSI_IRQ_CPU
	{	int i;
		for (i = 0; i < MAX_THREADS; i++) {
			handle_mw_reqest_cpu[i] = 0;
			handle_pf_reqest_cpu[i] = 0;
		}
	}
#endif
	}
}

#if GOD_VIEW
/***************
 * GOD VIEW - hash (SYS_REGION)
 */
void god_view_flip(struct seq_file *seq, void *v)
{
    if (seq) {
		is_god = !is_god;
		printk("is_god = %s\n", is_god ? "O" : "X");
	}
}

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
#if CONSERVATIVE
	/* performance reason - skip */
	memset(sys_region->read_addrs, 0, sizeof(sys_region->read_addrs));
	memset(sys_region->writenopg_addrs, 0, sizeof(sys_region->writenopg_addrs));
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
		/* wait till all followers are in the wq */
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
	} else { /* do sth efficient before sleeping? */
		DEFINE_WAIT(wait);
		prepare_to_wait_exclusive(&rc->waits_begin, &wait, TASK_UNINTERRUPTIBLE);
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
	int size2 = (sizeof(*req) - (sizeof(*req->pf_reqs) * MAX_PF_REQ)) // wroking // remove
								+ (sizeof(*req->pf_reqs) * pf_nr_pages);
	int size = (sizeof(*req) - sizeof(req->pf_reqs)) // testing
							+ (sizeof(*req->pf_reqs) * pf_nr_pages);
	BUG_ON(size != size2); // testing // remove
#if STRONG_CHECK_SANITY
	{
		if (MAX_PF_REQ == pf_nr_pages)
			BUG_ON(size != sizeof(*req));
	}
#endif

	BUG_ON(!req || !pf_nr_pages);

	/* pf_map */
	pf_map->pf_req_id = send_id;
	pf_map->pf_list_size = pf_nr_pages;
    spin_lock(&rc->pf_ongoing_lock);
	list_add_tail(&pf_map->list, &rc->pf_ongoing_list);
    spin_unlock(&rc->pf_ongoing_lock);

	/* msg */
	req->origin_pid = current->pid;
	//req->remote_pid = current->origin_pid;
    req->remote_pid = rc->remote_tgids[TO_THE_OTHER_NID()];

	req->god_omp_hash = __god_region_hash(sys_region->id, sys_region->cnt);

	req->pf_req_id = send_id;
	req->pf_list_size = pf_nr_pages;
	memcpy(req->pf_reqs, pf_reqs, sizeof(*req->pf_reqs) * pf_nr_pages);

#if 0
#define MAX_NAME 255
	char str[MAX_NAME]; int i, ofs = 0; bool err = false;
#if 0
	memset(str, 0, MAX_NAME);
	ofs += snprintf(str, MAX_NAME, "1. addrs[%d/%d]: ", pf_nr_pages, sizeof(*req->pf_reqs) * pf_nr_pages);
	for (i = 0; i < pf_nr_pages; i++) {
		ofs += snprintf(str + ofs, MAX_NAME, "0x%lx ", req->pf_reqs[i].addr);
		if (!req->pf_reqs[i].addr ||
			req->pf_reqs[i].addr == 0xafafafafafafafaf)
			err = true;
	}
	printk("%s\n", str);
#endif
	ofs = 0;
	memset(str, 0, MAX_NAME);
	ofs += snprintf(str, MAX_NAME, "2. addrs[%d/%d]: ", pf_nr_pages, sizeof(struct prefetch_list_body) * pf_nr_pages);
	for (i = 0; i < pf_nr_pages; i++) {
		ofs += snprintf(str + ofs, MAX_NAME, "0x%lx ", pf_reqs[i].addr);
		if (pf_reqs[i].addr != req->pf_reqs[i].addr) {
			printk("pf_reqs[%d].addr 0x%lx req->pf_reqs[%d].addr 0x%lx\n",
					i, pf_reqs[i].addr,
					i, req->pf_reqs[i].addr);
		}
		if (!pf_reqs[i].addr ||
			pf_reqs[i].addr == 0xafafafafafafafaf)
			err = true;
	}
	printk("%s\n", str);
	BUG_ON(err);
#endif
#if STRONG_CHECK_SANITY
	{	int i;
		for (i = 0; i < pf_nr_pages; i++) {
			BUG_ON(pf_reqs[i].addr != req->pf_reqs[i].addr);
			BUG_ON(!pf_reqs[i].addr);
		}
	}
#endif

	pcn_kmsg_post(PCN_KMSG_TYPE_REMOTE_PREFETCH_REQUEST,
							TO_THE_OTHER_NID(), req, size);

	PFPRINTK("\t\t\t->->#%d[/%d] 0x%lx ->->%d hash 0x%lx size %d fence %lu\n",
									send_id, pf_nr_pages,
									req->pf_reqs[pf_nr_pages - 1].addr,
									TO_THE_OTHER_NID(),
					__god_region_hash(sys_region->id, sys_region->cnt), size,
					current->tso_fence_cnt);
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
	PFVPRINTK("\t\t<-got reqs hash 0x%lx #%d\n",
				req->god_omp_hash, req->pf_req_id);

	handle_prefetch(req);

#if RCSI_IRQ_CPU
	handle_pf_reqest_cpu[raw_smp_processor_id()]++;
#endif

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
	PFVPRINTK("\t\t<-done reqs hash 0x%lx #%d\n",
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
	PFVPRINTK("\t\t  touched 0x%lx #%d\n",
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
	int sent_cnt = 0;
	int pf_nr_pages = 0;
	int pending_pf_req = 0;
	struct pf_ongoing_map *pf_map;
	struct prefetch_list_body pf_reqs[MAX_PF_REQ];
	int r_cnt = sys_region->read_cnt;		// statis / dbg
#if PF_TIME
	ktime_t pf_kmalloc_start, pf_kmalloc_done, dt;
#endif

#ifdef CONFIG_POPCORN_CHECK_SANITY
	BUG_ON(atomic_read(&current->mm->remote->pf_ongoing_cnt));
#endif

	hash_for_each_safe(sys_region->every_rregion_hash, bkt, tmp, fi, hentry) {
		unsigned long addr = fi->addr;
		struct fault_handle *fh = select_prefetch_page(addr);
#if STRONG_CHECK_SANITY
		BUG_ON(!addr);
#endif
		r_cnt--;
		if (!fh)
			continue;

		sent_cnt++;

#if PF_TIME
		if (!pf_nr_pages)
			pf_kmalloc_start = ktime_get();
#endif
		BUG_ON(!pf_map);
		/* on going req map */
		if (!pf_nr_pages)
			pf_map = kzalloc(sizeof(*pf_map), GFP_KERNEL);

#if PF_TIME
		if (!pf_nr_pages) {
			pf_kmalloc_done = ktime_get();
			dt = ktime_sub(pf_kmalloc_done, pf_kmalloc_start);
			pf_malloc_time += ktime_to_ns(dt);
		}
#endif
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
#if !SKIP_MEM_CLEAN // testing
			/* performance reason - skip */
			memset(pf_reqs, 0,
				sizeof((sizeof(struct prefetch_list_body) * MAX_PF_REQ)));
#endif
		}

		/* msg limitation */
		if (pending_pf_req >= MAX_PF_MSG)
			break;
	}
	if (pf_nr_pages) { // 1 ~ 6
		__send_pf_req(pf_nr_pages, pending_pf_req, pf_reqs, pf_map, sys_region);
		pending_pf_req++;
	}

	if (pending_pf_req >= MAX_PF_MSG) {
		sys_region->skip_r_cnt_msg_limit += r_cnt;
#if PERF_FULL_BULL_WARN
		PCNPRINTK_ERR("*** skip - msg limit ***\n");
#endif
		goto out; // skip every_wregion_hash
	} else {
#if STRONG_CHECK_SANITY
		BUG_ON(r_cnt); /* if no skipping, check cnt == hash_size */
#endif
	}

#if 0
	int w_cnt = sys_region->writenopg_cnt; // statis / dbg
	hash_for_each_safe(sys_region->every_wregion_hash, bkt, tmp, fi, hentry) {
		//TODO every_wregion_hash
		//TODO every_wregion_hash
		//TODO every_wregion_hash
		//TODO every_wregion_hash
		//TODO every_wregion_hash
		//TODO every_wregion_hash
		w_cnt--;
	}
#endif

out:
	PFPRINTK("-->--> sent_cnt %d (%d msgs) == r_cnt %d (w_cnt %d) "
					"hash 0x%lx -->-->\n", sent_cnt, pending_pf_req,
					sys_region->read_cnt, sys_region->writenopg_cnt,
				__god_region_hash(sys_region->id, sys_region->cnt));
#if PF_TIME
	if (pending_pf_req) {
		int pf_pgs = pf_nr_pages + ((pending_pf_req - 1) * MAX_PF_REQ);
		if (pf_pgs > pf_pgs_peak)
			pf_pgs_peak = pf_pgs;
		pf_pgs_total += pf_pgs;
		pf_pgs_cnt++;

		if (pending_pf_req > pf_send_peak)
			pf_send_peak = pending_pf_req;
		pf_send_total += pending_pf_req;
		pf_send_cnt++;
	}
#endif
	return pending_pf_req;
}

/*  stop-my-world prefetch (others may not stopped!!!) */
void __prefetch(struct sys_omp_region *sys_region)
{
	int pending_pf_req;
#if PF_TIME
	ktime_t pf_send, pf_wait_fixup, pf_start = ktime_get();
#endif
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
			sys_region->read_cnt,
			sys_region->writenopg_cnt,
			current->tso_fence_cnt);

	/* issue prefetchs & wait till all done */
	pending_pf_req = __select_prefetch_pages_send(sys_region);
#if PF_TIME
	pf_send = ktime_get();
#endif
	__wait_prefetch_req_done(pending_pf_req);
#if PF_TIME
	pf_wait_fixup = ktime_get();

	pf_time_wait_send += ktime_to_ns(ktime_sub(pf_send, pf_start));
	pf_time_wait_fixup += ktime_to_ns(ktime_sub(pf_wait_fixup, pf_send));
#endif
}

bool __may_prefetch(struct sys_omp_region *sys_region)
{
	//if ((sys_region->read_cnt + sys_region->writenopg_cnt) < PREFETCH_THRESHOLD ||
	if (sys_region->read_cnt < PREFETCH_THRESHOLD) {
#if PF_TIME
		pf_threshold_skip_pgs += sys_region->read_cnt;
#endif
		return false;
	}
	if (sys_region->read_cnt <= 0) // no hist
		return false;

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
				sys_region->read_cnt,
				sys_region->writenopg_cnt,
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

	if (!sys_region) { /* create sys_region */
		sys_region =
			__add_sys_omp_region_hash(region_hash_cnt_hash, omp_hash, region_cnt);
		//printk("create hash 0x%lx 0x%lx %d - %p\n",
		//		region_hash_cnt_hash, omp_hash, region_cnt, sys_region);
#if STRONG_CHECK_SANITY
		BUG_ON(!__sys_omp_region_hash(region_hash_cnt_hash, omp_hash, region_cnt));
#endif
	}
}

void __dbg_si_addrs(unsigned long omp_hash, int region_cnt)
{
	unsigned long region_hash_cnt_hash = __god_region_hash(omp_hash, region_cnt);
	struct sys_omp_region *sys_region =
		__sys_omp_region_hash(region_hash_cnt_hash, omp_hash, region_cnt);
	if (!sys_region) // !region_cnt fall through
		return;
	BUG_ON(!region_hash_cnt_hash);

	if (sys_region->read_cnt
		//+ sys_region->writenopg_cnt // will take w into consideration
		>= PREFETCH_THRESHOLD &&
		sys_region->read_cnt > 0) {
#if 1
		PFPRINTK("[%d] 0x%lx #%d: hash 0x%lx pg_cnt r %d w %d fence %lu\n",
				current->pid, sys_region->id, region_cnt,
				__god_region_hash(sys_region->id, sys_region->cnt),
				sys_region->read_cnt,
				sys_region->writenopg_cnt,
				current->tso_fence_cnt);
#endif
	}
}

void __show_si_addrs(struct remote_context *rc)
{
#if STATIS // put for all variables
	struct hlist_node *tmp;
	struct sys_omp_region *sys_region;
	int bkt, region_cnt = 0, r_cnt = 0, w_cnt = 0;
	int r_max = 0, w_max = 0;
	int skip_r_cnt = 0, skip_w_cnt = 0;
	int skip_r_cnt_msg_limit = 0, skip_w_cnt_msg_limit = 0;
	hash_for_each_safe(sys_region_hash, bkt, tmp, sys_region, hentry) {
		r_cnt += sys_region->read_cnt;
		w_cnt += sys_region->writenopg_cnt;
		if (r_max < sys_region->read_cnt)
			r_max = sys_region->read_cnt;
		if (w_max < sys_region->writenopg_cnt)
			w_max = sys_region->writenopg_cnt;

		skip_r_cnt += sys_region->skip_r_cnt;
		skip_w_cnt += sys_region->skip_w_cnt;

		skip_r_cnt_msg_limit += sys_region->skip_r_cnt_msg_limit;
		skip_w_cnt_msg_limit += sys_region->skip_w_cnt_msg_limit;
		region_cnt++;
	}

	printk("=========================================\n");
	printk("=== god view(pf) iter all sys_regions ===\n");
	printk("=========================================\n");
	/* log */
	printk("sys_region_cnt %d pg_cnt r %d(max %d) w %d (max %d)\n",
							region_cnt, r_cnt, r_max , w_cnt, w_max);
	/* perf */
	printk("\t\t\tbuf limit skip r %d w %d\n", skip_r_cnt, skip_w_cnt);
	printk("\t\t\tmsg limit skip r %d w %d\n",
			skip_r_cnt_msg_limit, skip_w_cnt_msg_limit);

	/* mistakes */
	printk("\t\t\tmistake wrong_hist %d wrong_hist_no_vma %d\n",
							rc->wrong_hist, rc->wrong_hist_no_vma);


	/* TODO */
	printk("\n");
	printk("TODO: 123\n");
	//printk("TODO: get pure pf data\n");
	printk("TODO: seperate __create_analyze_region()\n");
	printk("TODO: RDMA W will be faster?\n");
	printk("TODO: check - \"testing/test\"\n");
	//printk("TODO: show max/avf local_wr_cnt => multithread to make ARM node efficient? -> change to use post threadhold = 96 and sperated to 96 * n (1 at lease) peice\n");
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
#if CONFIG_X86_64
	long int divider = X86_THREADS;
#else
	long int divider = ARM_THREADS;
#endif
	printk("[0x%lx] %s:%d type 0x%lx in_reg_inv_sum %d in_reg_conf_sum %d "
			" (Regions) %d "
			"ompr->total_R %d ompr->skip_R %d [ReadFault] cnt %d\n",
			//"ompr->total_R %d/t %d ompr->skip_R %d/t %d [ReadFaulf] cnt %d\n",
			pos->id, pos->name, pos->line,
			pos->type, pos->in_region_inv_sum,
			pos->in_region_conflict_sum, pos->cnt,
			atomic_read(&pos->total_cnt) / divider,
			atomic_read(&pos->skip_cnt) / divider,
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
	__show_si_addrs(rc);
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

/**
 * tso_end_barrier(begin) & leader
 * the only entry creating "region"
 *
 */ // can delay but before rc region info cleaned
struct omp_region *__create_analyze_region(struct remote_context *rc)
{
	unsigned long omp_hash = current->tso_region_id;
	struct omp_region *region = __omp_region_hash_add(rc, omp_hash);
#if GOD_VIEW
//	unsigned long region_hash_cnt_hash =
//				__god_region_hash(current->omp_hash, current->region_cnt);
//	struct sys_omp_region *sys_region =
//				__sys_omp_region_hash(region_hash_cnt_hash,
//										current->omp_hash, current->region_cnt);
#endif
	BUG_ON(!omp_hash);
	BUG_ON(!region);

#if SMART_REGION_DBG
	region->in_region_inv_sum += rc->inv_cnt;
#endif
	if (!(region->type & RCSI_VAIN)) {
#if GOD_VIEW
//		if (!sys_region) return region;
#endif
		if (rc->inv_cnt < VAIN_THRESHOLD
#if GOD_VIEW
//			//&& sys_region->read_cnt + sys_region->writenopg_cnt
//			&& sys_region->read_cnt < PREFETCH_THRESHOLD
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
	atomic_add(current->skip_wr_per_rw_cnt, &rc->sys_smart_skip_cnt);
	//current->smart_skip_cnt += current->skip_wr_per_rw_cnt;
	current->skip_wr_per_rw_cnt = 0;
}


/************
 * Prefetch
 */
/* TODO: add writefault + !page_mine */
/* TODO: add writefault + !page_mine */
/* TODO: add writefault + !page_mine */
/* This is just loging */
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
#if TSO_TIME
	ktime_t mw_log_start = ktime_get();
#endif
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
		if (my_nid != NOCOPY_NODE) { /* check clean? */
			memcpy(rcsi_w_new->paddr, paddr, PAGE_SIZE);
		}

		/* TODO: atomic 1 line - start */
		spin_lock(&rc->inv_lock);
		ivn_cnt_tmp = rc->inv_cnt++;
		spin_unlock(&rc->inv_lock);
#if !SKIP_MEM_CLEAN
		BUG_ON(rc->inv_addrs[ivn_cnt_tmp]);
#endif
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
#else // !HASH_GLOBAL
    spin_lock(&rc->inv_lock);
	ivn_cnt_tmp = rc->inv_cnt++; /* TODO: atomic 1 line */
    spin_unlock(&rc->inv_lock);

	//BUG_ON(rc->inv_addrs[ivn_cnt_tmp]);
	rc->inv_addrs[ivn_cnt_tmp] = addr;
	if (my_nid != NOCOPY_NODE) {
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
#if TSO_TIME
	{
		ktime_t dt, mw_log_end = ktime_get();
		dt = ktime_sub(mw_log_end, mw_log_start);
		BUG_ON(ktime_to_ns(dt) > (unsigned long)((unsigned long)10 * (unsigned long)NANOSECOND));
		atomic64_add(ktime_to_ns(dt), &mw_log_time);
	}
#endif
}

/********
 * Duplication
 */
struct sys_omp_region *__si_inc_common(struct task_struct *tsk)
{
#if PREFETCH && GOD_VIEW
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

/* read & write into 1*/
void si_read_inc(struct task_struct *tsk, unsigned long addr)
{
#if PF_TIME
	ktime_t pf_log_start = ktime_get();
#endif
#if PREFETCH && GOD_VIEW
	int read_slot, read_cnt;
	struct fault_info *fi;
	struct sys_omp_region *sys_region = __si_inc_common(tsk);
	if (!is_god)
		return;

	if (!sys_region)
		return;

	/* save if not duplicated */
	spin_lock(&sys_region->r_lock);

	read_cnt = sys_region->read_cnt;
	if (read_cnt >=
			sizeof(sys_region->read_addrs) /
			sizeof(*sys_region->read_addrs)) {
		sys_region->skip_r_cnt++;
#if PERF_FULL_BULL_WARN
		printk("si_r_addrs_buf full\n");
#endif
		goto out;
	}

	/* peek */
	hash_for_each_possible(sys_region->every_rregion_hash, fi, hentry, addr)
		if (fi->addr == addr)
			goto out;

	/* save */
	fi = kmalloc(sizeof(*fi), GFP_ATOMIC); // perf - pg fault path
	BUG_ON(!fi); /* remember to free */
	fi->addr = addr;
	hash_add(sys_region->every_rregion_hash, &fi->hentry, addr);
	read_slot = read_cnt;
	sys_region->read_addrs[read_slot] = addr; /* g_arry */ //kill
	sys_region->read_cnt++;
#if STRONG_CHECK_SANITY
	BUG_ON(!addr);
#endif

out:
	spin_unlock(&sys_region->r_lock);
#endif
#if PF_TIME
	{
		ktime_t dt, pf_log_end = ktime_get();
		dt = ktime_sub(pf_log_end, pf_log_start);
		BUG_ON(ktime_to_ns(dt) > (unsigned long)((unsigned long)10 * (unsigned long)NANOSECOND));
		atomic64_add(ktime_to_ns(dt), &pf_log_time);
	}
#endif
}

/* read&write into 1*/
void si_writenopg_inc(struct task_struct *tsk, unsigned long addr)
{
#if PREFETCH && GOD_VIEW
	//bool populated = false; // TODO kfree(fi);
	int writenopg_slot, writenopg_cnt;
	struct fault_info *fi;
	struct sys_omp_region *sys_region = __si_inc_common(tsk);
	if (!is_god)
		return;

	if (!sys_region)
		return;

	/* save if not duplicated */
	spin_lock(&sys_region->w_lock);

	writenopg_cnt = sys_region->writenopg_cnt;
	if (writenopg_cnt >=
			sizeof(sys_region->writenopg_addrs) /
			sizeof(*sys_region->writenopg_addrs)) {
		sys_region->skip_w_cnt++;
#if PERF_FULL_BULL_WARN
		printk("si_r_addrs_buf full\n");
#endif
		goto out;
	}

	/* peek */
	hash_for_each_possible(sys_region->every_wregion_hash, fi, hentry, addr)
		if (fi->addr == addr)
			goto out;

	/* save */
	fi = kmalloc(sizeof(*fi), GFP_ATOMIC); // perf - pg fault path
	BUG_ON(!fi); /* remember to free */
	fi->addr = addr;
	hash_add(sys_region->every_wregion_hash, &fi->hentry, addr);
	writenopg_slot = writenopg_cnt;
	sys_region->writenopg_addrs[writenopg_slot] = addr; /* g_arry */ // kill
#if STRONG_CHECK_SANITY
	BUG_ON(!addr);
#endif
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
		if (my_nid != NOCOPY_NODE) { // SKIP_MEM_CLEAN can be skipped right? perf
#if !SKIP_MEM_CLEAN //testing
			memset(pos->paddr, 0, PAGE_SIZE); /* TODO: perf critical */
#endif
		}
		__put_rcsi_work(pos);
		hlist_del(&pos->hentry);
	}
	BUG_ON(!hash_empty(rcsi_hash));
	//spin_unlock(&rcsi_hash_lock);

#if !SKIP_MEM_CLEAN //testing
	memset(rc->inv_addrs, 0, sizeof(*rc->inv_addrs) * rc->inv_cnt);
#endif
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
	if (my_nid != NOCOPY_NODE)
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
	struct wait_station *ws = get_wait_station(current);
	int i;
	int iter = 0;
	int total_iter = 0;
	int sent_cnt = 0; /* aka total_sent_cnt */
	bool zero_case;
	int local_wr_cnt;
#if CONFIG_X86_64
	int other_node_cpus = ARM_THREADS;
#else
	int other_node_cpus = X86_THREADS;
#endif
	/* max inv addr per msg */
#if MW_CONSIDER_CPU
	int total_sent_cnt = 0; /* aka sent_cnt */
	int per_inv_batch_max = MAX_WRITE_INV_BUFFERS;
#else
	int single_sent = 0;
	//int per_inv_batch_max = MAX_WRITE_INV_BUFFERS;
	int per_inv_batch_max = 2000 / other_node_cpus; // 2000 is from SP huristic //testing
#endif
	int new_outer_iter;
#if MW_TIME
	ktime_t mw_find_conf_wait_scatters, mw_find_conf_start_t = ktime_get();
	ktime_t mw_find_conf_sends;
	ktime_t mw_find_conf_wait_res;
	ktime_t mw_find_conf_wait_applys;
#endif
	BUG_ON(rc->inv_cnt > MAX_ALIVE_THREADS * MAX_WRITE_INV_BUFFERS);

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

//	req->merge_id = rc->local_merge_id;
	SYNCPRINTK2("mg: wait l lr(%d/%d) scatter_pendings %d\n", rc->local_merge_id,
					rc->remote_merge_id, atomic_read(&rc->scatter_pendings));

	while (atomic_read(&rc->scatter_pendings))
		CPU_RELAX;
#if STRONG_CHECK_SANITY
	BUG_ON(atomic_read(&rc->scatter_pendings)); // remove
#endif
	SYNCPRINTK2("mg: pass* l lr(%d/%d) scatter_pendings %d\n", rc->local_merge_id,
					rc->remote_merge_id, atomic_read(&rc->scatter_pendings));
#if MW_TIME
	mw_find_conf_wait_scatters = ktime_get();
#endif

	/* general */
	/* scatters - send - even send when 0 */
	if (local_wr_cnt == 0) { /* special zero case 0/0 */
		zero_case = true;
		iter = 0; /* total == 0 iter == 0 */
		total_iter = 0;
		new_outer_iter = 0;
		atomic_inc(&rc->scatter_pendings);
		// current->tso_nobenefit_region_cnt++; /* TODO: use rc and function */
		MWCPUPRINTK("msg %d notifying\n", total_iter);
	} else if (local_wr_cnt > 0) {
		zero_case = false;
		iter++; /* total > 0 iter start from 1 */

#if STRONG_CHECK_SANITY
		for (i = 0; i < local_wr_cnt; i++)
			BUG_ON(!rc->inv_addrs[i]);
#endif

#if !MW_CONSIDER_CPU
		total_iter = ((local_wr_cnt - 1) / per_inv_batch_max) + 1;
		for (i = 0; i < total_iter; i++)
			atomic_inc(&rc->scatter_pendings);
#endif

/** version1 - cpu oriented part1 **/
#if MW_CONSIDER_CPU
		/* new machanism */
		// calculate every should send pgs and msgs
		new_outer_iter = local_wr_cnt / (per_inv_batch_max * other_node_cpus);

		// do dry run (see if merge code)
		for (i = 0; i < other_node_cpus; i++) {
			int pgs_to_send, ongo_pgs_to_send;
			int per_sent_cnt = 0;
			int reminder = 0;
			if (i < local_wr_cnt % other_node_cpus)
				reminder = 1;
			pgs_to_send = ongo_pgs_to_send = (new_outer_iter * per_inv_batch_max) +
					((local_wr_cnt % ((per_inv_batch_max * other_node_cpus))) /
												other_node_cpus) + reminder;
			MWCPUPRINTK("\t++ pgs_to_send %d\n", pgs_to_send);
			if (!pgs_to_send) break;
			while (ongo_pgs_to_send) {
				int single_sent = ongo_pgs_to_send;
				//pgs_to_send - per_sent_cnt;
				if (single_sent > per_inv_batch_max)
					single_sent = per_inv_batch_max;

				atomic_inc(&rc->scatter_pendings);
				total_iter++;

				per_sent_cnt += single_sent;
				total_sent_cnt += single_sent;
				ongo_pgs_to_send -= single_sent;
#if STRONG_CHECK_SANITY
				MWCPUPRINTK("\t\tsingle_sent %d ongo_pgs_to_send %d pgs_to_send %d\n",
								single_sent , ongo_pgs_to_send, pgs_to_send);
				BUG_ON(ongo_pgs_to_send < 0);
#endif
			}
		}
#if STRONG_CHECK_SANITY
		if (total_sent_cnt != local_wr_cnt) {
			MWCPUPRINTK("total_sent_cnt %d != local_wr_cnt %d",
							total_sent_cnt, local_wr_cnt);
			BUG_ON(total_sent_cnt != local_wr_cnt);
		}
#endif
		MWCPUPRINTK("msg %d aka scatter_pendings\n", total_iter);
#endif

#if MW_TIME
		if (local_wr_cnt > mw_inv_peak)
			mw_inv_peak = local_wr_cnt;
		mw_inv_total += local_wr_cnt;
		mw_inv_cnt++;

		if (total_iter > mw_send_peak)
			mw_send_peak = total_iter;
		mw_send_total += total_iter;
		mw_send_cnt++;
#endif
		// not zero case done
	} else { BUG(); }

/** version1 - cpu oriented part2 **/
#if MW_CONSIDER_CPU
	// do real run (see if merge code)
	//sent_cnt = 0
	for (i = 0; i < other_node_cpus; i++) {
		// all - local_wr_cnt
		//		- sent_cnt
		// per -
		// 		sent: per_sent_cnt (on going cnt)
		//		pgs_to_send: per cpu (total)
		//		ongo_pgs_to_send: per cpu (dynamic)
		int pgs_to_send, ongo_pgs_to_send;
		int per_sent_cnt = 0;
		int reminder = 0;
		if (i < local_wr_cnt % other_node_cpus)
			reminder = 1;
		pgs_to_send = ongo_pgs_to_send = (new_outer_iter * per_inv_batch_max) +
					((local_wr_cnt % ((per_inv_batch_max * other_node_cpus))) /
													other_node_cpus) + reminder;
		MWCPUPRINTK("cpu %d - to_send %d new_out_iter %d\n",
						i, pgs_to_send, new_outer_iter);
		if (!pgs_to_send && !zero_case) break; /* detect first not zero case 0 */
		while (ongo_pgs_to_send || zero_case) { // may skip directly aka !pgs_to_send continue
			page_merge_request_t *req = pcn_kmsg_get(sizeof(*req));
			//int single_sent = pgs_to_send - per_sent_cnt;
			int single_sent = ongo_pgs_to_send;
			if (single_sent > per_inv_batch_max)
				single_sent = per_inv_batch_max;

#if STRONG_CHECK_SANITY
			BUG_ON(!req || single_sent > PCN_KMSG_MAX_PAYLOAD_SIZE);
			BUG_ON(zero_case && (single_sent || iter || total_iter));
#endif

			/* common */
			req->origin_pid = current->pid;
			req->origin_ws = ws->id;
			req->remote_pid = rc->remote_tgids[nid];

			/* put more handshake info to detect skew cases */
			//req->begin =
			//req->end =
			req->merge_id = rc->local_merge_id;
			req->fence = current->tso_fence_cnt;

			/* specific */
			req->wr_cnt = single_sent;
			req->iter = iter; /* start from 1 */
			req->total_iter = total_iter;

			if (single_sent)
				memcpy(req->addrs, rc->inv_addrs + sent_cnt, // per region cnt
									sizeof(*req->addrs) * single_sent);

#if STRONG_CHECK_SANITY
			{ 	int k;
				for (k = 0; k < single_sent; k++)
					BUG_ON(!req->addrs[k]);
				/* local_wr and rc->inv_addrs are correct BUT req->addrs WRONG!! */
			}
#endif

			pcn_kmsg_post(PCN_KMSG_TYPE_PAGE_MERGE_REQUEST, nid, req,
									sizeof(*req) - sizeof(req->addrs) +
									(sizeof(*req->addrs) * single_sent));

			per_sent_cnt += single_sent;
			sent_cnt += single_sent; // per region cnt
			ongo_pgs_to_send -= single_sent;

			BARRMPRINTK("[%d]/%d: cpu %d / this %d / per_sent %d / to_send %d "
							"<- global -> "
							"sent(ofs) %d / local_wr_cnt %d MERGE req ->\n",
							iter, total_iter, i, single_sent, per_sent_cnt,
							ongo_pgs_to_send, sent_cnt, local_wr_cnt);

#if STRONG_CHECK_SANITY
			BUG_ON(ongo_pgs_to_send < 0);
#endif

			iter++;
			if (zero_case) break;
		}
		// done 1 cpu
		if (zero_case) break;
	}
#if STRONG_CHECK_SANITY
	if (sent_cnt != local_wr_cnt) {
		MWCPUPRINTK("sent_cnt %d != local_wr_cnt %d",
								sent_cnt, local_wr_cnt);
		BUG_ON(sent_cnt != local_wr_cnt);
	}
#endif
#endif


/** version2 - batch oriented **/
#if !MW_CONSIDER_CPU
	do {
		// lud arm 	mw_inv peak 210465 (in the begining 2nd region) avg 479
		page_merge_request_t *req = pcn_kmsg_get(sizeof(*req));
		BUG_ON(!req);
		single_sent = local_wr_cnt - sent_cnt;

		/* determine per inv batch size */
		if (single_sent > per_inv_batch_max)
			single_sent = per_inv_batch_max;
#if STRONG_CHECK_SANITY
		BUG_ON(single_sent > PCN_KMSG_MAX_PAYLOAD_SIZE);
#endif

		/* common */
		req->origin_pid = current->pid;
		req->origin_ws = ws->id;
		req->remote_pid = rc->remote_tgids[nid];

		req->merge_id = rc->local_merge_id;

		/* put more handshake info to detect skew cases */
		//req->begin =
		req->fence = current->tso_fence_cnt;
		//req->end =
		req->origin_ws = ws->id;

		req->wr_cnt = single_sent; /* !!! */
		req->iter = iter;
		req->total_iter = total_iter;

		/* optmization: remove this copy by moving into the func() */
		/* read from "rc->inv_addrs" now. optimize - remove rc->inv_addrs */
		if (single_sent)
			memcpy(req->addrs, rc->inv_addrs + sent_cnt,
								sizeof(*req->addrs) * single_sent);

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
		 * double check > 2k & < 2k
		 */
		pcn_kmsg_post(PCN_KMSG_TYPE_PAGE_MERGE_REQUEST, nid, req,
		//pcn_kmsg_send(PCN_KMSG_TYPE_PAGE_MERGE_REQUEST, nid, req,
								sizeof(*req) - sizeof(req->addrs) +
								(sizeof(*req->addrs) * single_sent));
		WSYPRINTK("\t  -> MERGE sent l#%d %d/%d\n",
					rc->local_merge_id, iter, total_iter);

		iter++;
		sent_cnt += single_sent;

		if (zero_case)	/* redundant? */
			break;
	} while (sent_cnt < local_wr_cnt);
#endif
#if MW_TIME
	mw_find_conf_sends = ktime_get();
#endif

	WSYPRINTK("\t  -> MERGE sent go to sleep l id %d - wait %d mg res msgs "
							"- ws %p\n", rc->local_merge_id, total_iter, ws);

	/* wait until the last scatter requestion done + diff_req__from_remote done */
	//kfree(req);
	wait_at_station(ws);
#if MW_TIME
	mw_find_conf_wait_res = ktime_get();
#endif

	/* wait all on going apply_diffs done (make msg handler clean) */
	if (my_nid == NOCOPY_NODE)
		while (atomic_read(&rc->doing_diff_cnt))
			CPU_RELAX;
#if MW_TIME
	mw_find_conf_wait_applys = ktime_get();
#endif

#if MW_TIME
	mw_time_find_conf_wait_scatters += ktime_to_ns(ktime_sub(
						mw_find_conf_wait_scatters, mw_find_conf_start_t));
	mw_time_find_conf_wait_sends += ktime_to_ns(ktime_sub(
						mw_find_conf_sends, mw_find_conf_wait_scatters));
	mw_time_find_conf_wait_res += ktime_to_ns(ktime_sub(
						mw_find_conf_wait_res, mw_find_conf_sends));
	mw_time_find_conf_wait_applys += ktime_to_ns(ktime_sub(
						mw_find_conf_wait_applys, mw_find_conf_wait_res));
#endif
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
	//page_diff_apply_request_t *req = kmalloc(sizeof(*req), GFP_KERNEL);
	page_diff_apply_request_t *req = pcn_kmsg_get(sizeof(*req)); // testing
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
    pcn_kmsg_post(PCN_KMSG_TYPE_PAGE_DIFF_APPLY_REQUEST,
    //pcn_kmsg_send(PCN_KMSG_TYPE_PAGE_DIFF_APPLY_REQUEST,
								nid, req, sizeof(*req));
	//kfree(req);
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
//static int handle_page_apply_diff_request(struct pcn_kmsg_message *msg)
static void process_page_apply_diff_request(struct work_struct *work)
{
	START_KMSG_WORK(page_diff_apply_request_t, req, work);
    //page_diff_apply_request_t *req = (page_diff_apply_request_t *)msg;
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
	DIFFPRINTK("[%d/%5d] <- locally applying diff 0x%lx\n",
			tsk->pid, atomic_read(&rc->req_diffs), req->diff_addr);

	__put_task_remote(rc);
    put_task_struct(tsk);
	//pcn_kmsg_done(req);
	//return 0;
    END_KMSG_WORK(req);
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
			SYNCPRINTK2("\t\t\trlist: 0x%lx\n", wr_addrs[i]);

		for (i = 0; i < rc->inv_cnt; i++)
			SYNCPRINTK2("\t\t\tllist: 0x%lx\n", rc->inv_addrs[i]);
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
		/* !lock - concurrent read only */
		struct rcsi_work *rcsi_w = __rcsi_hash(req_addr);
		if (rcsi_w)
			conflict = true;
#else // remove
		for (j = 0; j < rc->inv_cnt; j++) {
			unsigned long addr = rc->inv_addrs[j];
			if (req_addr == addr) {
				conflict = true;
				break;
			}
		}
#endif //remove

		if (!req_addr) {
			printk(KERN_ERR "\t\t\t[ar/%d/%3d] %3s 0x%lx\n", my_nid, i, "inv", req_addr);
			BUG_ON(!req_addr);
		}

		if (!conflict) {
			/* optimize: delay */
			SYNCPRINTK3("\t\t\t[ar/%d/%3d] %3s 0x%lx\n", my_nid, i, "inv", req_addr);
			ret = do_locally_inv_page(tsk, req_addr);
			if (ret)
				BUG();
		} else { /* both will detect the same conflict addrs */
			conflict_cnt++; /* maintain meta to sync */
			SYNCPRINTK3("\t\t\t[ar/%d/%3d] %3s 0x%lx\n", my_nid, i, "ww", req_addr);
			if (my_nid != NOCOPY_NODE) {
				/* optimize: perf: batch */
				ret = do_locally_inv_page(tsk, req_addr);
				if (ret)
					BUG();
				__make_diffs_send_back_at_remote(tsk, rc, j, from_nid, req_addr);
			} else { /* I'm the owner */
				if (my_nid == 0) {
					sync_set_page_owner(0, tsk->mm, req_addr);
					sync_clear_page_owner(1, tsk->mm, req_addr);
				} else if (my_nid == 1) {
					sync_set_page_owner(1, tsk->mm, req_addr);
					sync_clear_page_owner(0, tsk->mm, req_addr);
				}
			}
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
	SYNCPRINTK2("\t\t\t%s: L:%d R:%d\n", __func__,
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
	BARRMPRINTK("\t\t\tto find conflict at remote req->wr_cnt %d (wait rc->ready)\n", wr_cnt);
	/* RC: make sure global list is ready - wait on cache line */
	//printk("remote mg start - 2\n");
	while(!rc->ready) {
		CPU_RELAX;
		smp_rmb(); // potential BUG(); TODO testing important
		/* This sleeping is somehow causing pf deadlock */
	}
	/* TODO bug spinging on bare now */
	BARRMPRINTK("\t\t\tdone find conflict at remote req->wr_cnt %d (barrier)\n", wr_cnt);
}

/* -> [Remote](multiple) diffs + merg_res => order matters */
static void process_page_merge_request(struct work_struct *work)
{
	START_KMSG_WORK(page_merge_request_t, req, work);
    page_merge_response_t *res = pcn_kmsg_get(sizeof(*res)); // testing
    //page_merge_response_t *res = kmalloc(sizeof(*res), GFP_KERNEL);
	struct task_struct *tsk = __get_task_struct(req->remote_pid);
	struct remote_context *rc = tsk->mm->remote;
	int conflict_cnt;
	int wr_cnt = req->wr_cnt;
	unsigned long *wr_addrs = req->addrs;
#if STRONG_CHECK_SANITY
	BUG_ON(!tsk || !res|| !rc);
#endif

	/* put more handshake info to detect skew cases */
	//req->begin =
	//current->tso_begin_m_cnt
	//current->tso_begin_cnt
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
	BARRMPRINTK("\t\t\t{{{ process conflict_cnt at remote %d/%d }}}\n",
										conflict_cnt, wr_cnt);

	res->origin_pid = req->origin_pid;
    res->origin_ws = req->origin_ws;
    //res->remote_pid = req->remote_pid;

	/* not only 1 page man... 32k-4 8-1 = 7diffs......*/
//	res->merge_id = iter_num;

	res->wr_cnt = req->wr_cnt;
	res->iter = req->iter;
	res->total_iter = req->total_iter;
	res->conflict_cnt = conflict_cnt; /* make sure origin will wait until all done. origin will calculate and get this same number */

	WSYPRINTK("\t\t\t<- process MERGE request [%d/%d] [%s] lr %d/%d\n",
					req->iter, req->total_iter,
					req->iter == req->total_iter ? "*" : "",
					rc->local_merge_id,
					rc->remote_merge_id);

	/* causion: more than RR */
	/* optimize: send_size (now > 1 pg) */
    pcn_kmsg_post(PCN_KMSG_TYPE_PAGE_MERGE_RESPONSE,
    //pcn_kmsg_send(PCN_KMSG_TYPE_PAGE_MERGE_RESPONSE,
					PCN_KMSG_FROM_NID(req), res, sizeof(*res));
	//kfree(res);

#if RCSI_IRQ_CPU
	handle_mw_reqest_cpu[raw_smp_processor_id()]++;
#endif

    put_task_struct(tsk);
    END_KMSG_WORK(req);
}


/* handshak -> [remote] */
static int handle_page_diff_all_done_request(struct pcn_kmsg_message *msg)
{
    remote_baiier_done_request_t *req = (remote_baiier_done_request_t *)msg;
	/* TODO: change name */
	struct task_struct *tsk = __get_task_struct(req->origin_pid);
	struct remote_context *rc = tsk->mm->remote;

	rc->remote_done = true;
	rc->remote_type = req->remote_region_type;

	rc->remote_done_cnt++; /* remote barrier */
	smp_wmb();
	WSYPRINTK("\t  <- handshake lr(%d/%d)\n",
					rc->local_done_cnt, rc->remote_done_cnt);

    put_task_struct(tsk);
	pcn_kmsg_done(req);
	return 0;
}


// * 1. fixup (async single now)
/* -> back[Local](main)(many_scatters) */
static int handle_page_merge_response(struct pcn_kmsg_message *msg)
//static void process_page_merge_response(struct work_struct *work)
{
    page_merge_response_t *res = (page_merge_response_t *)msg;
    //START_KMSG_WORK(page_merge_response_t, res, work);
    struct wait_station *ws = wait_station(res->origin_ws);
	struct task_struct *tsk = __get_task_struct(res->origin_pid);
	struct remote_context *rc = tsk->mm->remote;
	int remote_req_merge_left = atomic_dec_return(&rc->scatter_pendings);
	int i;
	BUG_ON(!rc || !tsk);

	//res->wr_cnt = req->wr_cnt;
	//res->iter = req->iter;
	//res->total_iter = req->total_iter;

	for (i = 0; i < res->conflict_cnt; i++)
		atomic_inc(&rc->doing_diff_cnt);

//done:
	if (!remote_req_merge_left) { /* last MERGE response */
		WSYPRINTK("\t<- MERGE response #%d [*] ws %p rmid %d\n",
							res->iter, ws, rc->remote_merge_id);
		/* wake up leader thread then wait diffs done */
		/* rc->doing_diff_cnt will not dangle around 0 */
		complete(&ws->pendings);
	} else if (remote_req_merge_left > 0) {
		WSYPRINTK("\t<- MERGE response #%d [ ] \n", res->iter);
	} else BUG();

    put_task_struct(tsk);
    //END_KMSG_WORK(res);
    pcn_kmsg_done(res);
    return 0;
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
				current->pid, "BEGIN", id, current->tso_begin_m_cnt,
				current->tso_begin_cnt, current->tso_fence_cnt, rc->threads_cnt);
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
	SYNCPRINTK2("- wait lr(%d/%d) diffs %d req_diffs %d remote_done %d\n",
				rc->local_done_cnt, rc->remote_done_cnt,
				atomic_read(&rc->diffs), atomic_read(&rc->req_diffs),
													rc->remote_done);
	while (rc->local_done_cnt > rc->remote_done_cnt) { /* wait for remote */
		CPU_RELAX;
		smp_rmb(); // potential BUG(); TODO testing important
	}
	/* at this moment - remote might have sent to me MERGE req */
	/* race condition rc-> diffs is 0 but reset by later -1 */
	WSYPRINTK("- *pass handshake lr(%d/%d)\n",
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
#if MW_TIME
		ktime_t mw_end, mw_start = ktime_get();
#endif
		BARRPRINTK("=== ---[*%d] BARRIER %5s! line %d mb %lu b %lu f %lu t %d "
					"Hand Shake done. ===\n\n",
					current->pid, "END", id, current->tso_begin_m_cnt,
					current->tso_begin_cnt, current->tso_fence_cnt, rc->threads_cnt);

		//if(!current->at_remote){PFVPRINTK("= *BARRIER wait =\n");}
		__wait_end_followers(rc);
		__wait_remote_done_handshake(rc);
#if MW_TIME
		mw_end = ktime_get();
		BUG_ON(ktime_to_ns(ktime_sub(mw_end, mw_start)) >
				(unsigned long)((unsigned long)10 * (unsigned long)NANOSECOND));
		mw_time_wait_remotehs += ktime_to_ns(ktime_sub(mw_end, mw_start));
#endif
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
				current->pid, "END*", id, current->tso_begin_m_cnt,
				current->tso_begin_cnt, current->tso_fence_cnt, rc->threads_cnt);
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

void current_info_transfer_to_worker(void)
{
	struct remote_context *rc = get_task_remote(current);

	if (!rc->tso_begin_cnt && current->tso_begin_cnt)
		rc->tso_begin_cnt = current->tso_begin_cnt;
	if (rc->tso_begin_cnt && current->tso_begin_cnt)
		BUG_ON(rc->tso_begin_cnt != current->tso_begin_cnt);

	if (!rc->tso_fence_cnt && current->tso_fence_cnt)
		rc->tso_fence_cnt = current->tso_fence_cnt;
	if (rc->tso_fence_cnt && current->tso_fence_cnt)
		BUG_ON(rc->tso_fence_cnt != current->tso_fence_cnt);

	if (!rc->tso_end_cnt && current->tso_end_cnt)
		rc->tso_end_cnt = current->tso_end_cnt;
	if (rc->tso_end_cnt && current->tso_end_cnt)
		BUG_ON(rc->tso_end_cnt != current->tso_end_cnt);


	if (!rc->tso_begin_m_cnt && current->tso_begin_m_cnt)
		rc->tso_begin_m_cnt = current->tso_begin_m_cnt;
	if (rc->tso_begin_m_cnt && current->tso_begin_m_cnt)
		BUG_ON(rc->tso_begin_m_cnt != current->tso_begin_m_cnt);

	/* only leader */
//	if (!rc->tso_fence_m_cnt && current->tso_fence_m_cnt)
//		rc->tso_fence_m_cnt = current->tso_fence_m_cnt;
//	if (rc->tso_fence_m_cnt && current->tso_fence_m_cnt)
//		BUG_ON(rc->tso_fence_m_cnt != current->tso_fence_m_cnt);

	if (!rc->tso_end_m_cnt && current->tso_end_m_cnt)
		rc->tso_end_m_cnt = current->tso_end_m_cnt;
	if (rc->tso_end_m_cnt && current->tso_end_m_cnt)
		BUG_ON(rc->tso_end_m_cnt != current->tso_end_m_cnt);

	/* raw I put */
	if (!rc->kmpc_dispatch_ini && current->kmpc_dispatch_ini)
		rc->kmpc_dispatch_ini = current->kmpc_dispatch_ini;
	if (rc->kmpc_dispatch_ini && current->kmpc_dispatch_ini)
		BUG_ON(rc->kmpc_dispatch_ini != current->kmpc_dispatch_ini);

	if (!rc->kmpc_barrier_cnt && current->kmpc_barrier_cnt)
		rc->kmpc_barrier_cnt = current->kmpc_barrier_cnt;
	if (rc->kmpc_barrier_cnt && current->kmpc_barrier_cnt)
		BUG_ON(rc->kmpc_barrier_cnt != current->kmpc_barrier_cnt);
	if (!rc->kmpc_cancel_barrier_cnt && current->kmpc_cancel_barrier_cnt)
		rc->kmpc_cancel_barrier_cnt = current->kmpc_cancel_barrier_cnt;
	if (rc->kmpc_cancel_barrier_cnt && current->kmpc_cancel_barrier_cnt)
		BUG_ON(rc->kmpc_cancel_barrier_cnt != current->kmpc_cancel_barrier_cnt);

	if (!rc->kmpc_reduce && current->kmpc_reduce)
		rc->kmpc_reduce = current->kmpc_reduce;
	if (rc->kmpc_reduce && current->kmpc_reduce)
		BUG_ON(rc->kmpc_reduce != current->kmpc_reduce);

	if (!rc->kmpc_end_reduce && current->kmpc_end_reduce)
		rc->kmpc_end_reduce = current->kmpc_end_reduce;
	if (rc->kmpc_end_reduce && current->kmpc_end_reduce)
		if (rc->kmpc_end_reduce != current->kmpc_end_reduce)
			printk("rc->kmpc_end_reduce %lu current->kmpc_end_reduce %lu\n",
							rc->kmpc_end_reduce, current->kmpc_end_reduce);
		//BUG_ON(rc->kmpc_end_reduce != current->kmpc_end_reduce);

	if (!rc->kmpc_reduce_nowait && current->kmpc_reduce_nowait)
		rc->kmpc_reduce_nowait = current->kmpc_reduce_nowait;
	if (rc->kmpc_reduce_nowait && current->kmpc_reduce_nowait)
		BUG_ON(rc->kmpc_reduce_nowait != current->kmpc_reduce_nowait);

	if (!rc->kmpc_dispatch_fini && current->kmpc_dispatch_fini)
		rc->kmpc_dispatch_fini = current->kmpc_dispatch_fini;
	if (rc->kmpc_dispatch_fini && current->kmpc_dispatch_fini)
		BUG_ON(rc->kmpc_dispatch_fini != current->kmpc_dispatch_fini);

#if 0 // hhb hhcb hhbf in
//	if (!rc->hhb && current->hhb)
//		rc->hhb = current->hhb;
#endif
//	if (!rc-> && current->)
//		rc-> = current->;

	__put_task_remote(rc);
}

#include "../../msg_layer/ring_buffer.h"
void collect_tso_wr(struct task_struct *tsk)
{
#if STATIS // put for all variables
#if CONFIG_X86_64
	long int divider = X86_THREADS;
#else
	long int divider = ARM_THREADS;
#endif

	struct remote_context *rc = get_task_remote(tsk);
	printk("[%d]: %s (inv) all [[%lu]] = "
				"ww %d (remote ww [[%lu]]) + inv [[%lu]] "
				"smart_region_skip %d\n",
						tsk->pid, tsk->comm, rc->sys_rw_cnt,
						atomic_read(&rc->sys_ww_cnt),
						rc->remote_sys_ww_cnt,			// ww
						rc->sys_rw_cnt - rc->remote_sys_ww_cnt, // inv
						atomic_read(&rc->sys_smart_skip_cnt));
						/* ww_cnt is a bad name */
						//"violation_begin %d violation_end %d "
						//violation_begin, violation_end,

	printk("==============================\n");
	printk("=== MSG & other parameters ===\n");
	printk("==============================\n");
	printk("- MSG_POOL_SIZE = %d\n", MSG_POOL_SIZE);
	//printk("testing: increase rb size. RB_NR_CHUNKS 16\n");
	printk("TODO: test: increase rb size. current RB_NR_CHUNKS = %d\n", RB_NR_CHUNKS);
	printk("- max msg: %lu KB\n", (PCN_KMSG_MAX_SIZE >> 10));

	printk("- MAX_READ_BUFFERS %d\n", MAX_READ_BUFFERS);
	printk("- MAX_WRITE_NOPAGE_BUFFERS %d\n", MAX_WRITE_NOPAGE_BUFFERS);
	//printk(" %ld\n", );

	printk("- divider = %ld (my cpu cnt)\n", divider);
	printk("- MAX_THREADS %d\n", MAX_THREADS);

	printk("- FAULTS_HASH = %d\n", FAULTS_HASH);
	printk("- STRONG_CHECK_SANITY (%s)\n", STRONG_CHECK_SANITY ? "O" : "X");

	printk("- MAX_OMP_REGIONS %d\n", MAX_OMP_REGIONS);
	printk("- OMP_REGION_HASH_BITS %d\n", OMP_REGION_HASH_BITS);
	//printk(" (%s)\n",  ? "O" : "X");
	printk("- struct sys_omp_region = %lu B = %lu pgs\n",
						sizeof(struct sys_omp_region),
						sizeof(struct sys_omp_region) / PAGE_SIZE);
	printk("TODO: \"cat /proc/interrupts\" "
			"WQ is per thread -> check IRQ if same core "
								"-> WQ doese auto balance?\n");
	printk("\n");

	printk("===============\n");
	printk("===== tso =====\n");
	printk("===============\n");
	printk("(my)(__kmpc_dispatch_init tso_begin) begin %lu fence %lu end %lu(always 0)\n",
						rc->tso_begin_cnt, rc->tso_fence_cnt, rc->tso_end_cnt);
	printk("(raw) kmpc_dispatch_ini [[%lu]] kmpc_dispatch_fini [[%lu]]\n",
						rc->kmpc_dispatch_ini, rc->kmpc_dispatch_fini);
	printk("(raw) hhcb %lu <= (using) kmpc_cancel_barrier [[%lu]]\n",
							rc->hhcb, rc->kmpc_cancel_barrier_cnt);

	printk("\t\t hhb %lu <= (using) __kmpc_barrier [[%lu]] <= "
							"kmpc_reduce %lu + kmpc_end_reduce %lu\n",
							rc->hhb, rc->kmpc_barrier_cnt,
							rc->kmpc_reduce, rc->kmpc_end_reduce);
	printk("(raw)(next merge barrier) hhb %lu hhcb %lu hhbf %lu (TODO)\n",
									rc->hhb, rc->hhcb, rc->hhbf);

	printk("\n\n");
	printk("(raw)(__kmpc_for_static_init) begin_m %lu "
			"((not moved) Barriers)fence_m %lu "
			"(__kmpc_for_static_fini) end_m %lu\n",
			rc->tso_begin_m_cnt, rc->tso_fence_m_cnt, rc->tso_end_m_cnt);

	if (rc->tso_begin_m_cnt || rc->tso_fence_m_cnt || rc->tso_end_m_cnt)
		printk("\ttso: WARNNING - MISSING TO HANDLE SOME REGIONS !!!!!\n");
	else
		printk("\ttso: handled all regions NICE!\n");

	printk("(raw) kmpc_reduce_nowait %lu\n",
					rc->kmpc_reduce_nowait);
	printk("kmpc_reduce_nowait should be 0, otherwise have to try to remove from runtime\n");
	//#define HHB 30 //hierarchy_hybrid_barrier 30
	//#define HHCB 31 //hierarchy_hybrid_cancel_barrier 31
	//#define HHBF 32 //hierarchy_hybrid_barrier_final 32

	printk("\n");
	mw_results(rc);
	printk("\n");
	pf_results(rc);
	printk("\n");
	//printk("TODO: more info\n");
	//printk("TODO: 123\n");

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

	current->kmpc_dispatch_ini++;

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
		__region_total_cnt_inc(region);
		__parse_save_region_info(region, file);

		/* If region recorded, try to prefetch */
#if GOD_VIEW && PREFETCH
		{
			bool leader;
#if PF_TIME
			ktime_t pf_start;
#endif
			/* prefetch pages */
			leader = __start_begin_barrier();
			if (leader) {
				struct sys_omp_region *sys_region;
#if PF_TIME
				pf_start = ktime_get();
#endif
				/* serial pf */
				sys_region = __god_view_prefetch(rc, omp_hash, region->cnt);
				if (!sys_region) {
					write_lock(&sys_region_hash_lock);
					__ondemand_si_region_create(omp_hash, region->cnt);
					write_unlock(&sys_region_hash_lock);
				}
			}
			__end_begin_barrier(leader);

			current->omp_hash = omp_hash;
			current->region_cnt = region->cnt;
#if PF_TIME
			if (leader) {
				ktime_t dt, pf_end = ktime_get();
				dt = ktime_sub(pf_end, pf_start);
				BUG_ON(ktime_to_ns(dt) > (unsigned long)((unsigned long)10 * (unsigned long)NANOSECOND));
				pf_time += ktime_to_ns(dt);
			}
#endif
		}
#endif
#if SMART_REGION
		if (region->type & RCSI_VAIN) { /* smart wr */
			SMRPRINTK("[%d] %s(): omp_hash 0x%lx b_cnt %lu bm_cnt %lu "
						"no benefit **SKIP**\n", current->pid, __func__,
						omp_hash, current->tso_begin_cnt, current->tso_begin_m_cnt);
			__region_skip_cnt_inc(region);
			return 0;
		}
#endif
	}

	/* TODO: THINK HARD: Doing readfault related works here
		is costly since it has to stop all threads */

	current->tso_region = true;
	current->tso_begin_cnt++;
	RGNPRINTK("[%d] %s(): omp_hash 0x%lx b_cnt %lu bm_cnt %lu\n", current->pid,
				__func__, omp_hash, current->tso_begin_cnt, current->tso_begin_m_cnt);
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
		WSYPRINTK("wait diffs sync: l calc %d r calc %d (origin only) lr(%d/%d)\n",
					atomic_read(&rc->diffs), atomic_read(&rc->req_diffs),
					rc->local_merge_id, rc->remote_merge_id);
		while (atomic_read(&rc->diffs) != atomic_read(&rc->req_diffs)) {
			CPU_RELAX;
		}
		WSYPRINTK("*pass diffs sync: l calc %d r calc %d (origin only) lr(%d/%d)\n",
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
#if MW_TIME
	ktime_t mw_start;
	ktime_t mw_find_conf, mw_wait_diffs, mw_wait_merges;
#endif
#if SMART_REGION
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
#if MW_TIME
		mw_start = ktime_get();
#endif
		//if (current->tso_region){printk("MERGE wait\n");}
		__locally_find_conflictions(TO_THE_OTHER_NID(), rc);
		//if (current->tso_region){printk("MERGE passed\n");}
#if MW_TIME
		mw_find_conf = ktime_get();
#endif
#if GOD_VIEW
		// dbg - before region_cnt++ just in case
		__dbg_si_addrs(current->omp_hash, current->region_cnt);
#endif

		/* if never been here, create. todo - see if do outside leader (may be seperate analyze here & create outside). pf need the resgion info */
		region = __create_analyze_region(rc);
		region->cnt++;

		__wait_diffs_done(rc); /* till local == remote calculated # done local  */
#if MW_TIME
		mw_wait_diffs = ktime_get();
#endif
		__wait_merge_msgs(rc);
#if MW_TIME
		mw_wait_merges = ktime_get();
#endif

		/* conservertive order - after wait_merge() */
		__sned_handshake(rc, TO_THE_OTHER_NID(), region);
	}
	__popcorn_end_end_barrier(rc, id, leader, region);

#if MW_TIME
	if (leader) {
		ktime_t dt, mw_end = ktime_get();
		dt = ktime_sub(mw_end, mw_start);
#if STRONG_CHECK_SANITY
		//BUG_ON(ktime_to_ns(dt) > (unsigned long)((unsigned long)10 * (unsigned long)NANOSECOND));
#endif
		mw_time += ktime_to_ns(dt);
		mw_time_find_conf += ktime_to_ns(ktime_sub(mw_find_conf, mw_start));
		mw_time_wait_diffs += ktime_to_ns(ktime_sub(mw_wait_diffs, mw_find_conf));
		mw_time_wait_merges += ktime_to_ns(ktime_sub(mw_wait_merges, mw_wait_diffs));
		mw_time_wait_end += ktime_to_ns(ktime_sub(mw_end, mw_wait_merges));
	}
#endif
#endif

#if CONSERVATIVE
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

	if (a == KMPC_REDUCE) {
		current->kmpc_reduce++;
	} else if (a == KMPC_END_REDUCE) {
		current->kmpc_end_reduce++;
		//TODO try to skip
	} else if (a == KMPC_REDUCE_NOWAIT) {
		current->kmpc_reduce_nowait++;
		return 0;
	} else if (a == KMPC_DISPATCH_FINI) {
		current->kmpc_dispatch_fini++;
	}

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
	current->tso_end_cnt++;
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
	if (a == KMPC_BARRIER) {
		current->kmpc_barrier_cnt++;
	} else if (a == KMPC_CANCEL_BARRIER) {
		current->kmpc_cancel_barrier_cnt++;
	}

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
	current->tso_begin_m_cnt++;
	//printk("[%d] %s():\n", current->pid, __func__);
	//printk("[%d] %s():\n", current->pid, __func__);
	//return __popcorn_tso_begin(id, file, omp_hash, a, b);
	return 0;
}

SYSCALL_DEFINE5(popcorn_tso_fence_manual, int, id, void __user *, file, unsigned long, omp_hash, int, a, void __user *, b)
{
	current->tso_fence_m_cnt++;
	RGNPRINTK("[%d] %s():\n", current->pid, __func__);

	if (a == HHB) {
		current->mm->remote->hhb++;
	} else if (a == HHCB) {
		current->mm->remote->hhcb++;
	} else if (a == HHBF) {
		current->mm->remote->hhbf++;
	}

	//printk("[%d] %s():\n", current->pid, __func__);
	//return __popcorn_tso_fence(id, file, omp_hash, a, b);
	return 0;
}

SYSCALL_DEFINE5(popcorn_tso_end_manual, int, id, void __user *, file, unsigned long, omp_hash, int, a, void __user *, b)
{
	current->tso_end_m_cnt++;
	//printk("[%d] %s():\n", current->pid, __func__);
	//printk("[%d] %s():\n", current->pid, __func__);
	//return __popcorn_tso_end(id, file, omp_hash, a, b);
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

DEFINE_KMSG_WQ_HANDLER(page_merge_request); // sleep
DEFINE_KMSG_WQ_HANDLER(page_apply_diff_request);
#if GOD_VIEW
DEFINE_KMSG_WQ_HANDLER(remote_prefetch_request); // burst recv testing
DEFINE_KMSG_WQ_HANDLER(remote_prefetch_response); // gost lost // dbging
//DEFINE_KMSG_ORDERED_WQ_HANDLER(remote_prefetch_request); // solve burst recv
//DEFINE_KMSG_ORDERED_WQ_HANDLER(remote_prefetch_response);
#endif
/* all get post
owqowq works
pf req owq + pf res wq = silence (according to prev log -> pf response didn't scheduled by the wq)
pf req wq + pf res owq = o send 13 (remote out of RECV) + r send 12 (remote QP isn't available)
*/
/* after changing page_apply_diff_request to run on WQ

*/

int __init popcorn_sync_init(void)
{
	//__rcsi_hash_test();
	__rcsi_mem_alloc();

	REGISTER_KMSG_WQ_HANDLER(
            PCN_KMSG_TYPE_PAGE_MERGE_REQUEST, page_merge_request);
	//REGISTER_KMSG_HANDLER(	/* diff - one way - not sort */
	REGISTER_KMSG_WQ_HANDLER( // magic god - I guess INT cannot do something // but slow
			PCN_KMSG_TYPE_PAGE_DIFF_APPLY_REQUEST, page_apply_diff_request); // order wq
    REGISTER_KMSG_HANDLER(	/* main scatters */
            PCN_KMSG_TYPE_PAGE_MERGE_RESPONSE, page_merge_response);

	/* handshake */
    REGISTER_KMSG_HANDLER(	/* my side all done signal - one way - only one */
            PCN_KMSG_TYPE_PAGE_MERGE_DONE_REQUEST, page_diff_all_done_request);

#if GOD_VIEW
    //REGISTER_KMSG_HANDLER(	/* msg lost somehow */ /* main scatters */
    REGISTER_KMSG_WQ_HANDLER( // sleep
        PCN_KMSG_TYPE_REMOTE_PREFETCH_REQUEST, remote_prefetch_request);
    //REGISTER_KMSG_HANDLER(	/* msg lost somehow */ /* main scatters */
    REGISTER_KMSG_WQ_HANDLER( // fixup /* pg fixup may sleep */ // loose
        PCN_KMSG_TYPE_REMOTE_PREFETCH_RESPONSE, remote_prefetch_response);
#endif

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

#if RCSI_IRQ_CPU
	{	int i;
		for (i = 0; i < MAX_THREADS; i++) {
			handle_mw_reqest_cpu[i] = 0;
			handle_pf_reqest_cpu[i] = 0;
		}
	}
#endif
	/* larger MAX_MSG_SIZE -> more inv addr -> larger rc size */
	printk("sizeof(*rc) %lu\n", sizeof(struct remote_context));
	WARN_ON(sizeof(struct remote_context) > (PAGE_SIZE << (MAX_ORDER - 1)));
	printk("sizeof(*sys_region) %lu\n", sizeof(struct sys_omp_region));
	WARN_ON(sizeof(struct sys_omp_region) > (PAGE_SIZE << (MAX_ORDER - 1)));

	printk("TODO prealloc pf_ongoing_map pool: %lu\n",
							sizeof(struct pf_ongoing_map));

	/* dbg */
	DVLPRINTK("si: 8 * MAX_ALIVE_THREADS (%d) * "
			"MAX_WRITE_INV_BUFFERS (%d) = [%lu]/ PAGE = [%lu] pgs - "
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
	printk("%lu pgs\n", sizeof(struct sys_omp_region) / PAGE_SIZE);
	printk("%lu pgs\n", sizeof(struct sys_omp_region) / PAGE_SIZE);
	printk("%lu pgs\n", sizeof(struct sys_omp_region) / PAGE_SIZE);
	if (sizeof(struct sys_omp_region) > (1 << MAX_ORDER) * PAGE_SIZE) {
		printk("(1 << MAX_ORDER) * PAGE_SIZE %lu > struct sys_omp_region = %lu\n",
				 (1 << MAX_ORDER) * PAGE_SIZE, sizeof(struct sys_omp_region));
	}
	//BUG_ON(sizeof(struct sys_omp_region) > 1 << MAX_ORDER);

	return 0;
}
