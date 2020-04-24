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


/* KM/BLK pf cnt */
extern atomic64_t fp_cnt;

/* toggle */
#define TOGGLE_MEMORY_PATTERN_TRACE 70
bool is_trace_memory_pattern = false;

/* Force printk result */
#define FORCE_PRINTK_EXEC_TIME 99

// dbg (perf)
#define RCSI_IRQ_CPU 1
#if RCSI_IRQ_CPU
// msg cnt // not accurate
unsigned long handle_mw_reqest_cpu[MAX_POPCORN_THREADS];
unsigned long handle_pf_reqest_cpu[MAX_POPCORN_THREADS];
#endif

/* Brutal loging */
unsigned long mw_local_dispatch_calc_time = 0;
unsigned long mw_local_work_time = 0;

/* Popcorn global barrier */
atomic64_t popcorn_global_local_cnt = ATOMIC64_INIT(0); // can be local? right?
atomic64_t popcorn_global_remote_cnt = ATOMIC64_INIT(0);
#define GLOBAL_BARRIER_MANUAL_CLEAN 1
spinlock_t popcorn_global_lock;
spinlock_t popcorn_global_lock2;
/* TODO remove it */
//unsigned long popcorn_global_local_cnt = 0; // overflow problem
//unsigned long popcorn_global_remote_cnt = 0; // overflow problem
//volatile unsigned long popcorn_global_local_cnt = 0; // overflow problem
//volatile unsigned long popcorn_global_remote_cnt = 0; // overflow problem

atomic64_t popcorn_global_time = ATOMIC64_INIT(0);

/**
 * make MACRO
 */
/* smart skip */
unsigned long smart_skip_cnt = 0;
unsigned long real_sent_pf = 0;
unsigned long skip_sent_pf = 0;

unsigned long real_recv_pf_succ = 0;
unsigned long real_recv_pf_fail = 0;

/* handle remote req time */
atomic64_t mw_wait_last_done = ATOMIC64_INIT(0);
atomic64_t mw_wait_local_list_ready = ATOMIC64_INIT(0);
atomic64_t mw_find_collision_at_remote = ATOMIC64_INIT(0);

/************** working zone *************/

#define TRY_TO_FLIP_SMART_REGION 0
#define REGION_CHECK 0

#define SMART_REGION_PERF_DBG 0

#define SMART_REGION_DBG 0 /* to debug smart region working behaviour */

#define CURRENT_DEBUG 0
#if CURRENT_DEBUG
#define CURRPRINTK(...) printk(KERN_INFO __VA_ARGS__)
#else
#define CURRPRINTK(...)
#endif
/************** working zone *************/

/*** Perf critical logs ***/
#define FINAL_DATA 1
#define POST_ICDCS19_TESTING 0

#if FINAL_DATA
/*** perf - time statis ***/
#define PF_TIME 0
#define MW_TIME 0
#define TSO_TIME 0 // aka MW log time
#define TSO_LOG_TIME 0 // testing
#define GLOBAL_SYS_BAR_TIME 0
#define OUTSIDE_REGION_DEBUG 0 // popcorn_stat - technically overhead free
#else
#define PF_TIME 1
#define MW_TIME 1
#define TSO_TIME 1 // aka MW log time
#define TSO_LOG_TIME 1 // testing
#define GLOBAL_SYS_BAR_TIME 1
#define OUTSIDE_REGION_DEBUG 1
#endif


/*** cpu oriented req ***/
/* 1: cpu oriented 0: batch oriented */
#if VM_TESTING
#define MW_CONSIDER_CPU 1
#define PF_CONSIDER_CPU 0
#else
#define MW_CONSIDER_CPU 1 // TODO: use best ratio to test it
#define PF_CONSIDER_CPU 1 // TODO: implemente (not implemented)
#endif

#define CONSIDER_LOCAL_CPU 1

/*** minor features ****/
#define REENTRY_BEGIN_DISABLE 1 // 0: repeat show 1: once

/*** perf ***/
#define SKIP_MEM_CLEAN 1
#define CONSERVATIVE 0 // this is BETA 1:safe (icdcs: 0)
#define PERF_FULL_BULL_WARN 1 // only for prink statis still works and show in the end !!!

/* For mw workers */
struct workqueue_struct *mw_wq;
struct mw_work {
	struct work_struct work;
	void *my_work;
	int iter;
	int local_wr_cnt;
	struct remote_context *rc;
	int pgs_to_send;
	int send_ofs;
	int other_node_cpus;
	int nid;
	int total_iter; /* dry run require */

	/* from current (leader thread) */
	int pid;
	int tso_fence_cnt;

	struct wait_station *ws;

	int per_inv_batch_max;
};


/* To debug omp_hash collision between nodes or not
 *	Usage:
 * 		no one conflicts with BT
 *			run BT -> turn on -> run CG -> run SP
 *		CG SP left
 *			run CG -> turn on -> run SP
 *	(turn on = "echo > /proc/popcorn_stat manually")
 */
#define DBG_OMP_HASH 0 /* big lock */
#if DBG_OMP_HASH
#define OMP_HASH_DBG_CNT 100000
unsigned long omp_hash_dbg_arr[OMP_HASH_DBG_CNT];
bool omp_hash_dbg_start_find_conflict = false;
unsigned long omp_hash_dbg_oft = 0;
spinlock_t omp_hash_dbg_lock;
#endif
void show_omp_hash(unsigned long omp_hash)
{
#if DBG_OMP_HASH
	int i;
	bool skip = false;
	bool record = false;
	bool conflict = false;
	if (!omp_hash) return; // note sure WTh case

	spin_lock(&omp_hash_dbg_lock);
	if (!omp_hash_dbg_start_find_conflict) { // record

		for (i = 0; i < OMP_HASH_DBG_CNT; i++) {
			if (omp_hash_dbg_arr[i]) {
				if (omp_hash_dbg_arr[i] == omp_hash) {
					skip = true;
					break;	// has recorded skip
				} else {
					// exist + others
				}
			} else {
				omp_hash_dbg_arr[i] = omp_hash;
				if (omp_hash_dbg_oft != i) {
					printk("omp_hash_dbg_oft %lu i %d\n", omp_hash_dbg_oft, i);
					BUG();
				}
				omp_hash_dbg_oft++;
				record = true;
				break;
			}
		}
	} else { // start to find
		for (i = 0; i < omp_hash_dbg_oft; i++) {
			if (omp_hash_dbg_arr[i]) {
				if (omp_hash_dbg_arr[i] == omp_hash) {
					//printk("conflict %lu\n", omp_hash);
					conflict = true;
					break;
				}
			} else {
				BUG();
				break;
			}
		}
	}
	if (conflict) {
		printk(KERN_ERR "[%s] record (%s) omp_hash %lu conflict [[%s]] skip (%s)\n",
				omp_hash_dbg_start_find_conflict ? "O" : "X",
				record ? "O" : "X",
				omp_hash,
				conflict ? "O" : "X",
				skip ? "O" : "X"
				);
	}
	spin_unlock(&omp_hash_dbg_lock);
	BUG_ON(i == OMP_HASH_DBG_CNT);
#endif
}


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
#define WW_SYNC_VERBOSE_DBG 0 //
#if WW_SYNC_VERBOSE_DBG
#define WSYPRINTK(...) printk(KERN_INFO __VA_ARGS__)
#else
#define WSYPRINTK(...)
#endif


//#define MWCPU_DEBUG 1
#define MWCPU_DEBUG 0 //
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
#define BARRIER_INFO_MORE 0 //
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

/***
 * !perf
 */
#define DEVELOPE_DEBUG 1
#if DEVELOPE_DEBUG
#define DVLPRINTK(...) printk(KERN_INFO __VA_ARGS__)
#else
#define DVLPRINTK(...)
#endif

/* rcsi meta/mem hash lock per region (sys-wide) */
#define RCSI_HASH_BITS 10 /* TODO try larger */ // looks for MW
DEFINE_HASHTABLE(rcsi_hash, RCSI_HASH_BITS);
DEFINE_SPINLOCK(rcsi_hash_lock);

/* GOD VIEW */
#define SYS_REGION_HASH_BITS 10 /* TODO TODO TODO try larger */
DEFINE_HASHTABLE(sys_region_hash, SYS_REGION_HASH_BITS);
DEFINE_RWLOCK(sys_region_hash_lock);
bool is_god = true; /* 1: record 0: pf */

/* Violation section detecting */
#if REGION_CHECK
static bool print = false;
#endif

#if !SMART_REGION
static bool print_end = false;
#endif

static int violation_begin = 0;
static int violation_end = 0;

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
#define KMPC_DISPATCH_INIT 24

#define KMPC_STATIC_INIT 40
#define KMPC_STATIC_FINI 41

#define HHB 30 //hierarchy_hybrid_barrier 30
#define HHCB 31 //hierarchy_hybrid_cancel_barrier 31
#define HHBF 32 //hierarchy_hybrid_barrier_final 32

#define DISPATCH_NEXT_TO_STATIC_INIT 90

#define STATIC_INIT_TO_STATIC_SKEWED_INIT 80
#define STATIC_SKEWED_INIT 81

#if OUTSIDE_REGION_DEBUG
unsigned long longest_inside_region_time = 0;
unsigned long shortest_inside_region_time = 0;
bool track_outside_region = false;
ktime_t first_outside_region = {.tv64 = 0};
ktime_t last_outside_region = {.tv64 = 0};
#define OUTSIDE_REGION_START 60
#define OUTSIDE_REGION_END 61
#endif

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
#define EVERY_REGION_HASH_BITS_R 15 /* TODO testing this value */ // pf read
#define EVERY_REGION_HASH_BITS_W 10 /* TODO testing this value */ // pf w
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
#define MAX_STR 30
	/* dbg */
	/* readability */
	char name[MAX_STR];
	int line;
	/* statis */
	atomic_t total_cnt; // per thread
	atomic_t skip_cnt; // per thread
	int in_region_conflict_sum;
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
	//unsigned long read_addrs[MAX_READ_BUFFERS * MAX_POPCORN_THREADS / 1]; // hardcode TODO
	unsigned long read_addrs[MAX_READ_BUFFERS * MAX_POPCORN_THREADS / 4]; // hardcode TODO
	//unsigned long writenopg_addrs[MAX_WRITE_NOPAGE_BUFFERS * MAX_POPCORN_THREADS / 1]; // hardcode TODO
	unsigned long writenopg_addrs[1000]; // hardcode TODO
	//unsigned long read_addrs[MAX_READ_BUFFERS * MAX_POPCORN_THREADS / 5]; // hardcode TODO
	//unsigned long writenopg_addrs[MAX_WRITE_NOPAGE_BUFFERS * MAX_POPCORN_THREADS / 5]; // hardcode TODO

	/* two hash tables for addrs */
	DECLARE_HASHTABLE(every_rregion_hash, EVERY_REGION_HASH_BITS_R);
	rwlock_t every_rregion_hash_lock;
	DECLARE_HASHTABLE(every_wregion_hash, EVERY_REGION_HASH_BITS_W);
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
#ifdef CONFIG_X86_64
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
	printk("mw_time: [[%lu]]* s =>\n", mw_time / NANOSECOND);
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
	/* TODO: avg length per msg size */
	printk("mw_log_time %ld ms\n",
				atomic64_read(&mw_log_time) / MICROSECOND / divider);
	// handle for remote
	printk("mw handle remote req (s): wait_last_done %ld "
				"mw_wait_local_list_ready %ld mw_find_collision_at_remote %ld\n",
				atomic64_read(&mw_wait_last_done) / NANOSECOND,
				atomic64_read(&mw_wait_local_list_ready) / NANOSECOND,
				atomic64_read(&mw_find_collision_at_remote) / NANOSECOND);

	{	int i;
		printk("handle_mw_reqest_cpu: ");
		for (i = 0; i < MAX_POPCORN_THREADS; i++) {
			if (handle_mw_reqest_cpu[i])
				printk("[%d] %lu ", i, handle_mw_reqest_cpu[i]);
		}
		printk("\n");
	}
	printk("(my/tso)(actualperform) begin %lu fence %lu end %lu(always 0)\n",
				rc->tso_begin_cnt, rc->tso_fence_cnt, rc->tso_end_cnt);
	printk("== breakdown ==\n");
	printk("(actual) begin %lu  = "
			"(raw) kmpc_dispatch_init [[%lu]] kmpc_static_init [[%lu]]\n",
				rc->tso_begin_cnt,
				rc->kmpc_dispatch_init, rc->kmpc_static_init);
	printk("(actual) fence %lu  = "
			"kmpc_barrier_cnt (%lu) + kmpc_cancel_barrier (%lu)\n",
			rc->tso_fence_cnt,
			rc->kmpc_barrier_cnt, rc->kmpc_cancel_barrier_cnt);
	printk("(actual) end %lu  = "
			"(raw) kmpc_dispatch_fini [[%lu]] + kmpc_static_fini [[%lu]]\n",
			rc->tso_end_cnt,
			rc->kmpc_dispatch_fini, rc->kmpc_static_fini);
	printk("(don't care)(raw) kmpc_reduce %lu kmpc_end_reduce %lu "
			"kmpc_reduce_nowait %lu (0 when perf)\n",
			rc->kmpc_reduce, rc->kmpc_end_reduce, rc->kmpc_reduce_nowait);
	printk("\n");
//	printk("(raw) kmpc_dispatch_init [[%lu]] kmpc_dispatch_fini [[%lu]]\n",
//						rc->kmpc_dispatch_init, rc->kmpc_dispatch_fini);
//	printk("(raw) kmpc_static_init [[%lu]] kmpc_static_fini [[%lu]]\n",
//						rc->kmpc_static_init, rc->kmpc_static_fini);
	printk("(how many VAIN regions skipped)smart_skip_cnt %ld\n",
												smart_skip_cnt / divider);



	printk("TODO: WW merge one by one now -> more \n");
#endif
}

void pf_results(struct remote_context *rc)
{
#if PF_TIME
#ifdef CONFIG_X86_64
	long int divider = X86_THREADS;
#else
	long int divider = ARM_THREADS;
#endif
	printk("======================\n");
	printk("=== PF parameters ===\n");
	printk("======================\n");
	//printk("GOD_VIEW (%s)\n", GOD_VIEW ? "O" : "X");
	//printk("PREFETCH (%s)\n", PREFETCH ? "O" : "X");
	//printk(" (%s)\n",  ? "O" : "X");
	printk("- PREFETCH_THRESHOLD = max %lu pgs per msg\n", PREFETCH_THRESHOLD);
	printk("- MAX_PF_MSG %d\n", MAX_PF_MSG);
	printk("- PF_CONSIDER_CPU (%s) TODO: implemente\n",
							PF_CONSIDER_CPU  ? "O" : "X");

	printk("======================\n");
	printk("=== PF results ===\n");
	printk("======================\n");
	printk("real succ prefetch cnt pf_cnt [[%d]]!!\n",
						atomic_read(&rc->pf_succ_cnt));

	printk("pf_time: [[%lu]]* s =>\n", pf_time / NANOSECOND);
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

	/* skip pf */
	printk("\t\t\t(X)(Skipped because < %lu) "
			"pf_threshold_skip_pgs %lu (pgs)\n",
			PREFETCH_THRESHOLD, pf_threshold_skip_pgs);
	{	int i;
		printk("handle_pf_reqest_cpu: ");
		for (i = 0; i < MAX_POPCORN_THREADS; i++) {
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

#if GLOBAL_BARRIER_MANUAL_CLEAN
		printk("clean global_barrier cnts \t\t popcorn_global_local_cnt %ld\n"
									"\t\t popcorn_global_remote_cnt %ld\n"
									"\t\t and clean popcorn_global_time\n",
//									popcorn_global_local_cnt,
//									popcorn_global_remote_cnt);
									atomic64_read(&popcorn_global_local_cnt),
									atomic64_read(&popcorn_global_remote_cnt));

//		popcorn_global_local_cnt = 0;
//		popcorn_global_remote_cnt = 0;
		atomic64_set(&popcorn_global_local_cnt, 0);
		atomic64_set(&popcorn_global_remote_cnt, 0);

		atomic64_set(&popcorn_global_time, 0);
#endif

#if DBG_OMP_HASH
		omp_hash_dbg_start_find_conflict = !omp_hash_dbg_start_find_conflict;
		printk("omp_hash_dbg_start_find_conflict (%s)\n",
						omp_hash_dbg_start_find_conflict ? "O" : "X");
#endif

#if RCSI_IRQ_CPU
		{	int i;
			for (i = 0; i < MAX_POPCORN_THREADS; i++) {
				handle_mw_reqest_cpu[i] = 0;
				handle_pf_reqest_cpu[i] = 0;
			}
		}
#endif
		printk("(clean)(how many region skipped) smart_skip_cnt = 0\n");
		smart_skip_cnt = 0;
		printk("(clean)(real pf sent/skip) real_sent_pf = skip_sent_pf = 0\n");
		real_sent_pf = 0;
		skip_sent_pf = 0;

		printk("(clean)(real got pf succ/bad) "
				"real_recv_pf_succ = real_recv_pf_fail = 0\n");
		real_recv_pf_succ = 0;
		real_recv_pf_fail = 0;

		printk("(clean) violation_begin = violation_end = 0\n");
		violation_begin = 0;
		violation_end = 0;
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

#if 0 // TODO: post-icdcs removing locks
	/* pf_map */
	pf_map->pf_req_id = send_id;
	pf_map->pf_list_size = pf_nr_pages;
    spin_lock(&rc->pf_ongoing_lock);
	list_add_tail(&pf_map->list, &rc->pf_ongoing_list);
    spin_unlock(&rc->pf_ongoing_lock);
#endif // TODO: post-icdcs removing locks

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
extern struct fault_handle *select_prefetch_page2(unsigned long addr);
/***
 * return: pending_pf_req to wait
 */
int __select_prefetch_pages_send(struct sys_omp_region *sys_region)
{
	int bkt;
	struct fault_info *fi;
	struct hlist_node *tmp;
	int sent_cnt = 0;
	int pf_nr_pages = 0;
	int pending_pf_req = 0;
//	struct pf_ongoing_map *pf_map = NULL; // TODO: post-icdcs removing locks
	struct prefetch_list_body pf_reqs[MAX_PF_REQ];
#if STATIS
	int r_cnt = sys_region->read_cnt;
#endif
#if PF_TIME
	ktime_t pf_kmalloc_start, pf_kmalloc_done, dt;
#endif

#ifdef CONFIG_POPCORN_CHECK_SANITY
	BUG_ON(atomic_read(&current->mm->remote->pf_ongoing_cnt));
#endif

	hash_for_each_safe(sys_region->every_rregion_hash, bkt, tmp, fi, hentry) {
		unsigned long addr = fi->addr;
		struct fault_handle *fh = select_prefetch_page2(addr); // TODO: post-icdcs removing locks
#if STRONG_CHECK_SANITY
		BUG_ON(!addr);
#endif

#if STATIS
		r_cnt--;
#endif
		if (!fh) /* has owned the page */
			continue;

		sent_cnt++;

#if PF_TIME
		real_sent_pf++;
		if (!pf_nr_pages)
			pf_kmalloc_start = ktime_get();
#endif


#if 0 // TODO: post-icdcs removing locks
//		BUG_ON(!pf_map); // .........BUG
		/* on going req map */
		if (!pf_nr_pages)
			pf_map = kzalloc(sizeof(*pf_map), GFP_KERNEL);
#endif // TODO: post-icdcs removing locks

#if PF_TIME
		if (!pf_nr_pages) {
			pf_kmalloc_done = ktime_get();
			dt = ktime_sub(pf_kmalloc_done, pf_kmalloc_start);
			pf_malloc_time += ktime_to_ns(dt);
		}
#endif

#ifdef CONFIG_POPCORN_CHECK_SANITY
//		BUG_ON(!pf_map); // TODO: post-icdcs removing locks
#endif
		//pf_map->fh[pf_nr_pages] = fh; // TODO: post-icdcs removing locks (chage pf response)
		// nothing
//		pf_map->addr[pf_nr_pages] = addr; // TODO: post-icdcs removing locks

		/* preparing msg */
		pf_reqs[pf_nr_pages].addr = addr;
		pf_nr_pages++;
		// load to msg 1 ~ 7 (0-6)

		if (pf_nr_pages >= MAX_PF_REQ) { // >= 7 ([0-6]) send
			//__send_pf_req(pf_nr_pages, pending_pf_req, pf_reqs, pf_map, sys_region);
			__send_pf_req(pf_nr_pages, pending_pf_req, pf_reqs, NULL, sys_region); // TODO: post-icdcs removing locks
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
		//__send_pf_req(pf_nr_pages, pending_pf_req, pf_reqs, pf_map, sys_region); // TODO: post-icdcs removing locks
		__send_pf_req(pf_nr_pages, pending_pf_req, pf_reqs, NULL, sys_region); // TODO: post-icdcs removing locks
		pending_pf_req++;
	}

	if (pending_pf_req >= MAX_PF_MSG) {
#if STATIS
		sys_region->skip_r_cnt_msg_limit += r_cnt;
#endif
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
	skip_sent_pf += sys_region->read_cnt - sent_cnt;
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

	/*printk("[%d] 0x%lx #%d: (prefetch) "
			"hash 0x%lx r_cnt %d w_cnt %d fence %lu\n",
			current->pid, sys_region->id, sys_region->cnt,
			__god_region_hash(sys_region->id, sys_region->cnt),
			sys_region->read_cnt,
			sys_region->writenopg_cnt,
			current->tso_fence_cnt);*/
	PFPRINTK("[%d] 0x%lx #%d: (prefetch) "
			"hash 0x%lx r_cnt %d w_cnt %d fence %lu\n",
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
	/* Take siw into consideration */
	//if ((sys_region->read_cnt + sys_region->writenopg_cnt)
	//									< PREFETCH_THRESHOLD ||
	if (sys_region->read_cnt < PREFETCH_THRESHOLD) {
#if PF_TIME
		if (sys_region->read_cnt > 0)
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
	PFPRINTK("\t\tsys_region->read_cnt %d\n", sys_region->read_cnt);
	//printk("\t\tsys_region->read_cnt %d\n", sys_region->read_cnt);

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
		// for debuging pf collect this region's pf info
		// if see, matchup
		/*printk("[%d] 0x%lx #%d: (fence_dbg) "
				"hash 0x%lx pg_cnt r %d w %d fence %lu\n",
				current->pid, sys_region->id, region_cnt,
				__god_region_hash(sys_region->id, sys_region->cnt),
				sys_region->read_cnt,
				sys_region->writenopg_cnt,
				current->tso_fence_cnt);*/
		PFPRINTK("[%d] 0x%lx #%d: (fence_dbg) "
				"hash 0x%lx pg_cnt r %d w %d fence %lu\n",
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
#if STATIS_END // put for all variables
	struct hlist_node *tmp;
	struct sys_omp_region *sys_region;
	int bkt, pf_region_cnt = 0, r_cnt = 0, w_cnt = 0;
	int r_max = 0, w_max = 0;
	int skip_r_cnt = 0, skip_w_cnt = 0;
	int skip_r_cnt_msg_limit = 0, skip_w_cnt_msg_limit = 0;
	hash_for_each_safe(sys_region_hash, bkt, tmp, sys_region, hentry) {
		if (sys_region->read_cnt > 0)
			r_cnt += sys_region->read_cnt;
		if (sys_region->writenopg_cnt > 0)
			w_cnt += sys_region->writenopg_cnt;

		if (r_max < sys_region->read_cnt && sys_region->read_cnt > 0)
			r_max = sys_region->read_cnt;
		if (w_max < sys_region->writenopg_cnt &&
					sys_region->writenopg_cnt > 0)
			w_max = sys_region->writenopg_cnt;

		if (sys_region->skip_r_cnt > 0)
			skip_r_cnt += sys_region->skip_r_cnt;
		if (sys_region->skip_w_cnt > 0)
			skip_w_cnt += sys_region->skip_w_cnt;

		if (sys_region->skip_r_cnt_msg_limit > 0)
			skip_r_cnt_msg_limit += sys_region->skip_r_cnt_msg_limit;
		if (sys_region->skip_w_cnt_msg_limit > 0)
			skip_w_cnt_msg_limit += sys_region->skip_w_cnt_msg_limit;

		pf_region_cnt++;
	}

	printk("=========================================\n");
	printk("=== god view(pf) iter all sys_regions ===\n");
	printk("=========================================\n");
	/* log */
	printk("sys_pf_region_cnt %d pg_cnt r %d(max %d) w %d (max %d)\n",
							pf_region_cnt, r_cnt, r_max , w_cnt, w_max);
	/* real sent */
	printk("real sent/skip sent_pf %lu skip_sent_pf %lu\n",
						real_sent_pf, skip_sent_pf);
	/* real got */
	printk("real got good/bad real_recv_pf_succ %lu real_recv_pf_fail %lu\n",
										real_recv_pf_succ, real_recv_pf_fail);
	/* perf */
	printk("\t\t\tbuf limit skip r %d w %d\n", skip_r_cnt, skip_w_cnt);
	printk("\t\t\t(X)msg limit skip r %d w %d\n",
			skip_r_cnt_msg_limit, skip_w_cnt_msg_limit);

	/* mistakes */
	printk("\t\t\tmistake wrong_hist %d wrong_hist_no_vma %d\n",
							rc->wrong_hist, rc->wrong_hist_no_vma);


#if DBG_OMP_HASH
	printk("echo > /proc/popcorn_stat\n");
	printk("echo > /proc/popcorn_stat\n");
	printk("echo > /proc/popcorn_stat\n");
	printk("echo > /proc/popcorn_stat\n");
	printk("echo > /proc/popcorn_stat\n");
#endif
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

void __show_other_dbg_info(struct remote_context *rc)
{
	// move below to other region
	printk("\n");
	printk("=========================================\n");
	printk("=========== global barrier ===============\n");
	printk("=========================================\n");
	printk("popcorn_global_time [[ %ld ]] s\n",
				atomic64_read(&popcorn_global_time) / NANOSECOND);

	printk("\n");
	printk("================================================\n");
	printk("======= debuging for new omp behavior  =========\n");
	printk("================================================\n");
	printk("dispatch_next_to_static_init [[ %ld ]]\n",
						rc->dispatch_next_to_static_init);
	printk("static_init_to_static_skewed_init [[ %ld ]]\n",
						rc->static_init_to_static_skewed_init++);
	printk("static_skewed_init [[ %ld ]]\n",
						rc->static_skewed_init);

#if OUTSIDE_REGION_DEBUG
	printk("\n");
	printk("===========================================\n");
	printk("===== inside region time (auto reset) =====\n");
	printk("===========================================\n");
	printk("longest_inside_region_time [[ %lu ]] s %lu ns\n",
					longest_inside_region_time/1000/1000/1000,
									longest_inside_region_time);
	printk("shortest_inside_region_time [[ %lu ]] s %lu ns\n",
					shortest_inside_region_time/1000/1000/1000,
									shortest_inside_region_time);
	longest_inside_region_time = 0;
	shortest_inside_region_time = 0;
#endif
}

void __show_mwpf_config(void)
{
	printk("===========================================\n");
	printk("===== inside region time (auto reset) =====\n");
	printk("===========================================\n");
#if VM_TESTING
	printk("VM_TEST = 1\n");
#else
	printk("VM_TEST = 0\n");
#endif

#if MW_CONSIDER_CPU
	printk("MW_CONSIDER_CPU = 1\n");
#else
	printk("MW_CONSIDER_CPU = 0\n");
#endif

#if PF_CONSIDER_CPU
	printk("PF_CONSIDER_CPU = 1\n");
#else
	printk("PF_CONSIDER_CPU = 0\n");
#endif

#if GOD_VIEW
	printk("GOD_VIEW = 1\n");
#else
	printk("GOD_VIEW = 0\n");
#endif
#if PREFETCH
	printk("PREFETCH = 1\n");
#else
	printk("PREFETCH = 0\n");
#endif

#if FINAL_DATA
	printk("FINAL_DATA = 1\n");
#else
	printk("FINAL_DATA = 0\n");
#endif

	printk("TODO printk CONN\n");

	/* Move to other place*/
	printk("mw_local_dispatch_calc_time = %lu ns\n", mw_local_dispatch_calc_time);
	printk("mw_local_work_time = %lu ns\n", mw_local_work_time);
	printk("mw_local_dispatch_calc_time = %lu ms\n", mw_local_dispatch_calc_time/1000/1000);
	printk("mw_local_work_time = %lu ms\n", mw_local_work_time/1000/1000);
	mw_local_dispatch_calc_time = 0;
	mw_local_work_time = 0;
}

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
	smart_skip_cnt++;
}

void inline ______smart_region_debug(struct remote_context *rc, struct omp_region *pos)
{
#if SMART_REGION_DBG
#ifdef CONFIG_X86_64
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

	__show_other_dbg_info(rc);
	__show_mwpf_config();

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
	//region->cnt = 0; // TODO testing
	//////////////////// since position
	region->cnt = 1; // TODO testing
	//////////////////// since position
#if SMART_REGION_DBG
	atomic_set(&region->total_cnt, 1);
	atomic_set(&region->skip_cnt, 0);
	region->vain_cnt = 0;
	region->name[0] = 0;
	region->line = 0;
	region->in_region_conflict_sum = 0;
#endif
	region->read_fault_cnt = 0;
	region->read_max = 0;
	region->read_min = 0;
	region->in_region_inv_sum = 0;
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
				/* enter next begion WARN: skew will hang */
				// TODO not using flip now
#if SMART_REGION_DBG
				printk("[0x%lx] %llu clean RCSI_VAIN %d \n",
							region->id, current->tso_wr_cnt,
							current->tso_benefit_cnt);
#endif
			}
		}
	} else {
		current->tso_benefit_cnt = 0;
	}
}


/************
 * Prefetch
 */
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
#if !SKIP_MEM_CLEAN
	rcsi_w->addr = 0;
#endif
	spin_lock(&rcsi_bmap_lock);
    BUG_ON(!test_bit(id, rcsi_bmap));
    clear_bit(id, rcsi_bmap);
	spin_unlock(&rcsi_bmap_lock);
}

/************
 *  TSO
 * Per region. Will reset by __clean_global_rcsi()
 */
extern void maintain_origin_table(unsigned long target_addr);
void tso_wr_inc(struct vm_area_struct *vma, unsigned long addr, struct page *page, spinlock_t *ptl)
{
#if TSO_TIME
	ktime_t mw_log_start = ktime_get();
#endif
	void *paddr;
	int ivn_cnt_tmp;
	struct remote_context *rc = current->mm->remote;
#if HASH_GLOBAL
	struct rcsi_work *found, *rcsi_w_new;
#endif
	BUG_ON(!rc);

#if !GLOBAL /* implementation - local */
	//current->buffer_inv_addrs[current->tso_wr_cnt] = addr;
	//current->tso_wr_cnt++;
#else /* implementation - global */
#if HASH_GLOBAL
	rcsi_w_new = __get_rcsi_work();
	BUG_ON(!rcsi_w_new);

	spin_lock(&rcsi_hash_lock);
	found = __rcsi_hash(addr);
	if (likely(!found))
		hash_add(rcsi_hash, &rcsi_w_new->hentry, addr);
	spin_unlock(&rcsi_hash_lock);

	if (likely(!found)) { /* No local collision */
#if !SKIP_MEM_CLEAN
		BUG_ON(rcsi_w_new->addr);
#endif
		rcsi_w_new->addr = addr;
#if !MW_IDEAL
		if (my_nid != NOCOPY_NODE) { /* check clean? */
			paddr = kmap(page);
			BUG_ON(!paddr);
			memcpy(rcsi_w_new->paddr, paddr, PAGE_SIZE);
			kunmap(page);
		}
#endif
		/* TODO: atomic 1 line - start */
		spin_lock(&rc->inv_lock);		// TODO change to use atomic
		ivn_cnt_tmp = rc->inv_cnt++;	// TODO change to use atomic
		spin_unlock(&rc->inv_lock);		// TODO change to use atomic
#if !SKIP_MEM_CLEAN
		BUG_ON(rc->inv_addrs[ivn_cnt_tmp]);
#endif
		rc->inv_addrs[ivn_cnt_tmp] = addr; /* cnt to know max inv_addrs */

#if BUF_DBG
		SYNCPRINTK("[%d/%d] %s buf 0x%lx ins %lx %d\n", current->pid,
						ivn_cnt_tmp,
						my_nid == 0 ? "origin" : "remote",  addr,
						instruction_pointer(current_pt_regs()),
						rcsi_w_new->id);
#endif
		/* ownership update - from now on I own it */
		maintain_origin_table(addr);
	} else {
		printk(KERN_ERR "local collision\n");
		__put_rcsi_work(rcsi_w_new);
	}
#else // !HASH_GLOBAL
//    spin_lock(&rc->inv_lock);
//	ivn_cnt_tmp = rc->inv_cnt++; /* TODO: atomic 1 line */
//    spin_unlock(&rc->inv_lock);
//
//	rc->inv_addrs[ivn_cnt_tmp] = addr;
//#if !MW_IDEAL
//	if (my_nid != NOCOPY_NODE) {
//#if STRONG_CHECK_SANITY
//		BUG_ON(*(rc->inv_pages + (ivn_cnt_tmp * PAGE_SIZE)));
//#endif
//		memcpy(rc->inv_pages + (ivn_cnt_tmp * PAGE_SIZE), paddr, PAGE_SIZE);
//		//copy_from_user_page(vma, page, addr,
//		//		rc->inv_pages + (ivn_cnt_tmp * PAGE_SIZE), paddr, PAGE_SIZE);
//		kunmap(page);
//	}
//#endif

#endif
#endif

#if BUF_DBG
	SYNCPRINTK("[%d/%d] %s buf 0x%lx ins %lx\n", current->pid, ivn_cnt_tmp,
					my_nid == 0 ? "origin" : "remote",  addr,
					instruction_pointer(current_pt_regs()));
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
 * SI / duplication / prefetch
 */
struct sys_omp_region *__si_inc_common(struct task_struct *tsk)
{
#if PREFETCH && GOD_VIEW
	unsigned long omp_hash = tsk->omp_hash;
	int region_cnt = tsk->region_cnt;
	unsigned long region_hash_cnt_hash = __god_region_hash(omp_hash, region_cnt);
	struct sys_omp_region *sys_region =
		__sys_omp_region_hash(region_hash_cnt_hash, omp_hash, region_cnt);

	/* smar_region conflicts with pf problem */
//	if (!region_cnt) // first // covered by !sys_reigon
//		return NULL;;

	/* solve smar_region conflicts with pf problem */
	if (!tsk->smart_is_vain)	// smart!=vain  watchout // if(==) > pass
		if (!tsk->tso_region) // bug if samrt == vain.......
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
		WARN_ON_ONCE("godview si_r_addrs_buf full\n");
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
#if 0
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
		WARN_ON_ONCE("godview si_w_addrs_buf full\n");
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
void __maintain_local_ownership_serial(void)
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

/* This configure is not showen in the end */
/* consider remote core cnt (icdcs19 data) */
#ifdef CONFIG_X86_64
	int other_node_cpus = ARM_THREADS;
#else
	int other_node_cpus = X86_THREADS;
#endif
/* consider local core cnt */
//#ifdef CONFIG_X86_64
//	int other_node_cpus = X86_THREADS;
//#else
//	int other_node_cpus = ARM_THREADS;
//#endif
	/* max inv addr per msg */
#if MW_CONSIDER_CPU
	int total_sent_cnt = 0; /* aka sent_cnt */
	int per_inv_batch_max = MAX_WRITE_INV_BUFFERS;
#else
	int single_sent = 0;
	int per_inv_batch_max = 2000 / other_node_cpus; // 2000 is from SP huristic //testing
#endif
	int new_outer_iter;
#if MW_TIME
	ktime_t mw_find_conf_sends;
	ktime_t mw_find_conf_wait_res;
	ktime_t mw_find_conf_wait_applys;
	ktime_t mw_find_conf_wait_scatters, mw_find_conf_start_t = ktime_get();
#endif
	ktime_t mw_dispatch_calc_time_start;
	ktime_t mw_dispatch_calc_time_end;
	ktime_t mw_work_time_start;
	ktime_t mw_work_time_end;
	BUG_ON(rc->inv_cnt > MAX_ALIVE_THREADS * MAX_WRITE_INV_BUFFERS);

#if !GLOBAL
	#if 0
	/* implementation - local */
	local_wr_cnt = sync_server_local_conflictions(rc);
	/* dbg */
	if (rc->lconf_cnt)
		BARRMPRINTK("local_conflict_addr_cnt %d\n", rc->lconf_cnt);
	BUG_ON(rc->lconf_cnt > MAX_ALIVE_THREADS * MAX_WRITE_INV_BUFFERS);
	#endif
#else
#if HASH_GLOBAL
	/* no collistion */
	local_wr_cnt = rc->inv_cnt;
#if MW_TIME
	rc->sys_rw_cnt += rc->inv_cnt;
#endif
	/* redo ownership maintaining in serial phase for corner cases */
	__maintain_local_ownership_serial();
#else
	#if 0
	/* implementation - global */
	local_wr_cnt = sync_server_local_serial_conflictions(rc);
#if MW_TIME
	rc->sys_rw_cnt += local_wr_cnt;
#endif
	#endif
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

	/* Check inv reqs */
	if (local_wr_cnt == 0) {
		/* Case 1: I don't have any inv req (special zero case 0/0) */
		/* Send even when 0 inv req */
		zero_case = true;
		iter = 0; /* total == 0 iter == 0 */
		total_iter = 0;
		new_outer_iter = 0;
		atomic_inc(&rc->scatter_pendings);
		MWCPUPRINTK("msg %d notifying\n", total_iter);
	} else if (local_wr_cnt > 0) {
		/* Case 2: I have inv reqs */
		/***
		 * Work dispatch: dicide how many req/res to send/recv
		 */
		mw_dispatch_calc_time_start = ktime_get();
		zero_case = false;
		iter++; /* total > 0 iter start from 1 */

#if STRONG_CHECK_SANITY
		for (i = 0; i < local_wr_cnt; i++)
			BUG_ON(!rc->inv_addrs[i]);
#endif

/** work dispatch version2 - batch oriented part1 **/
#if !MW_CONSIDER_CPU
		total_iter = ((local_wr_cnt - 1) / per_inv_batch_max) + 1;
		for (i = 0; i < total_iter; i++)
			atomic_inc(&rc->scatter_pendings);
#endif

/** work dispatch version1 - cpu oriented part1 **/
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
		mw_dispatch_calc_time_end = ktime_get();
		mw_local_dispatch_calc_time +=
			ktime_to_ns(ktime_sub(mw_dispatch_calc_time_end,
									mw_dispatch_calc_time_start));
	} else { BUG(); }


/***
 * Real Work - v1 or v2
 */

/** Real work version1 - cpu oriented part2 **/
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

		mw_work_time_start = ktime_get();
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

			mw_work_time_end = ktime_get();
			mw_local_work_time +=
					ktime_to_ns(ktime_sub(mw_work_time_end,
											mw_work_time_start));
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
#endif /** MW_CONSIDER_CPU end **/


/** Real work version2 - batch oriented **/
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
#endif /** !MW_CONSIDER_CPU end **/

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

static void mw_parallel_func(struct work_struct *work)
{
	struct mw_work *mw_w = (struct mw_work *)(work);
//	mw_w->my_work;
//	mw_w->iter;
	/* per region */
	int local_wr_cnt = mw_w->local_wr_cnt;
	int total_iter = mw_w->total_iter; /* dry run required */

	struct remote_context *rc = mw_w->rc;
	int pgs_to_send = mw_w->pgs_to_send;
	int ongo_pgs_to_send = mw_w->pgs_to_send;
	int per_t_accu_sent_cnt = 0;
	int reminder = 0;
	int other_node_cpus = mw_w->other_node_cpus;
	int nid = mw_w->nid;
	int per_inv_batch_max = mw_w->per_inv_batch_max; // important
	/* dynamically changed */
	int iter = mw_w->iter; // iter = req_base // important // NEW // last guy's last #
	int per_t_sent_ofs = mw_w->send_ofs; // important

	if (iter < local_wr_cnt % other_node_cpus)
		reminder = 1;

	MWCPUPRINTK("from wq base_iter[%d] - per_t_sent_ofs %d - total %d invs",
									iter, per_t_sent_ofs, pgs_to_send);

#if STRONG_CHECK_SANITY
	BUG_ON(pgs_to_send <= 0);
	BUG_ON(per_t_sent_ofs > local_wr_cnt);
#endif

	while (ongo_pgs_to_send) { // may skip directly aka !pgs_to_send continue
		page_merge_request_t *req = pcn_kmsg_get(sizeof(*req));
		int single_sent = ongo_pgs_to_send;
		if (single_sent > per_inv_batch_max)
			single_sent = per_inv_batch_max;

	MWCPUPRINTK("\t\titer[%d] - sending per_t_sent_ofs %d total  %d invs",
									iter, per_t_sent_ofs, pgs_to_send);
#if STRONG_CHECK_SANITY
		BUG_ON(!req || single_sent > PCN_KMSG_MAX_PAYLOAD_SIZE ||
												single_sent <= 0);
#endif

		/* common */
		req->origin_pid = mw_w->pid;		// TODO // important //NEW
		req->origin_ws = mw_w->ws->id;
		req->remote_pid = rc->remote_tgids[nid];

		/* put more handshake info to detect skew cases */
		//req->begin =
		//req->end =
		req->merge_id = rc->local_merge_id;
		req->fence = mw_w->tso_fence_cnt; // important //NEW // TODO

		/* specific */
		req->wr_cnt = single_sent;
		req->iter = iter; /* start from 1 */
		req->total_iter = total_iter; /* required all done */

		memcpy(req->addrs, rc->inv_addrs + per_t_sent_ofs,
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

		per_t_accu_sent_cnt += single_sent;
		per_t_sent_ofs += single_sent; // per region global ofs
		ongo_pgs_to_send -= single_sent;

		BARRMPRINTK("[%d]/%d: (mw_parallel) this sent %d this accu sent %d "
						"per_t_sent_ofs %d <- global -> "
						"sent(g_ofs) %d/ %d(per t)/ local_wr_cnt %d(per phase) - "
						"%d left  MERGE req ->\n",
						iter, total_iter, single_sent, per_t_accu_sent_cnt,
						per_t_sent_ofs,
						per_t_sent_ofs, pgs_to_send, local_wr_cnt, ongo_pgs_to_send);
		iter++;
	}
	MWCPUPRINTK("iter[%d]@DONE@ - this thread has to_send %d invs",
											iter, pgs_to_send);
	kfree(work);
}

void __locally_find_conflictions2(int nid, struct remote_context *rc)
{
	struct wait_station *ws = get_wait_station(current);
	int i;
	int iter = 0;
	int total_iter = 0;
	bool zero_case;
	int local_wr_cnt;
#if !CONSIDER_LOCAL_CPU
#ifdef CONFIG_X86_64
	int other_node_cpus = ARM_THREADS;
#else
	int other_node_cpus = X86_THREADS;
#endif
#else
#ifdef CONFIG_X86_64
	int other_node_cpus = X86_THREADS;
#else
	int other_node_cpus = ARM_THREADS;
#endif
#endif
	/* max inv addr per msg */
#if MW_CONSIDER_CPU
	int per_inv_batch_max = MAX_WRITE_INV_BUFFERS;
#else
	int single_sent = 0;
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

	/* no collistion */
	local_wr_cnt = rc->inv_cnt;
#if MW_TIME
	rc->sys_rw_cnt += rc->inv_cnt;
#endif
	/* redo ownership maintaining in serial phase for corner cases */
	__maintain_local_ownership_serial();

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

	/* Check inv reqs */
	if (local_wr_cnt == 0) {
		/* Case 1: I don't have any inv req (special zero case 0/0) */
		/* Send even when 0 inv req */
		page_merge_request_t *req = pcn_kmsg_get(sizeof(*req));
		int single_sent = 0;
		int ongo_pgs_to_send = 0;

		zero_case = true;
		iter = 0; /* zero case - total == 0 iter == 0 */
		total_iter = 0; /* zero case */
		new_outer_iter = 0;
		atomic_inc(&rc->scatter_pendings);
		MWCPUPRINTK("msg %d notifying\n", total_iter);

		/* common */
		req->origin_pid = current->pid;
		req->origin_ws = ws->id;
		req->remote_pid = rc->remote_tgids[nid];

		/* put more handshake info to detect skew cases */
		//req->begin =
		//req->end =
		req->merge_id = rc->local_merge_id;
		req->fence = current->tso_fence_cnt;

		/* specific - zero case / not zero case */
		req->wr_cnt = single_sent;
		req->iter = iter;
		req->total_iter = total_iter; /* if not zero case - required dry run */

		pcn_kmsg_post(PCN_KMSG_TYPE_PAGE_MERGE_REQUEST, nid, req,
								sizeof(*req) - sizeof(req->addrs) +
								(sizeof(*req->addrs) * single_sent));

		//per_sent_cnt += single_sent;
		//per_t_sent_ofs += single_sent; // per region cnt
		//ongo_pgs_to_send -= single_sent;

		BARRMPRINTK("[%d]/%d: (%s) cpu %d / this %d / per_sent *%d / to_send %d "
						"<- global -> "
						"sent(ofs) *%d / local_wr_cnt %d MERGE req ->\n",
						iter, total_iter, "zero inv case",
						i, single_sent, single_sent,
						ongo_pgs_to_send, single_sent, local_wr_cnt);

	} else if (local_wr_cnt > 0) {
		/* Case 2: I have inv reqs */
		/***
		 * Work dispatch: dicide how many req/res to send/recv
		 */
		int total_sent_cnt = 0; /* aka sent_cnt - for accumulating */
		int total_sent_cnt_ofs[other_node_cpus]; /* aka sent_cnt ofs per t*/
		int pgs_to_send[other_node_cpus];
		int iter_ofs[other_node_cpus];
		for (i = 0; i < other_node_cpus; i++) {
			total_sent_cnt_ofs[i] = 0;
			pgs_to_send[i] = 0;
			iter_ofs[i] = 0;
		}

		zero_case = false;
		iter++; /* total > 0 iter start from 1 */
		//total_iter = 1; /* total > 0 iter start from 1 */ /* trick: [1] guy shold start from iter# 2*/

#if STRONG_CHECK_SANITY
		for (i = 0; i < local_wr_cnt; i++)
			BUG_ON(!rc->inv_addrs[i]);
#endif

/** work dispatch version1 - cpu oriented part1 dry run **/
#if MW_CONSIDER_CPU
		/* new machanism */
		// calculate every should send pgs and msgs
		new_outer_iter = local_wr_cnt / (per_inv_batch_max * other_node_cpus);

		// do dry run (see if merge code)
		for (i = 0; i < other_node_cpus; i++) {
			int ongo_pgs_to_send; // dynamically check - decreasing
			int per_sent_cnt = 0;
			int reminder = 0;
			if (i < local_wr_cnt % other_node_cpus)
				reminder = 1;
			pgs_to_send[i] = ongo_pgs_to_send = (new_outer_iter * per_inv_batch_max) +
					((local_wr_cnt % ((per_inv_batch_max * other_node_cpus))) /
												other_node_cpus) + reminder;
			MWCPUPRINTK("\tdispatching++ pgs_to_send[%d] %d / total mw %d\n",
									i, pgs_to_send[i], local_wr_cnt);
			if (!pgs_to_send[i]) break;
			while (ongo_pgs_to_send) {
				int single_sent = ongo_pgs_to_send;
				//pgs_to_send[i] - per_sent_cnt;
				if (single_sent > per_inv_batch_max)
					single_sent = per_inv_batch_max;

				atomic_inc(&rc->scatter_pendings);
				total_iter++;

				iter_ofs[i] = total_iter; /* NEW: my last iter # for the next guy */ /* 2nd guy should see 2 asumming 1st guy sent only 1 req */

				per_sent_cnt += single_sent;
				total_sent_cnt += single_sent;
				total_sent_cnt_ofs[i] = total_sent_cnt;
				ongo_pgs_to_send -= single_sent;
#if STRONG_CHECK_SANITY
				MWCPUPRINTK("\t\titer_ofs[%d] %d - "
							"single_sent %d ongo_pgs_to_send %d / "
							"pgs_to_send[%d] %d per parallel thread - "
							"generating total_sent_cnt_ofs[%d] %d\n",
							i, iter_ofs[i],
							single_sent, ongo_pgs_to_send,
							i, pgs_to_send[i],
							i, total_sent_cnt_ofs[i]);
				BUG_ON(ongo_pgs_to_send < 0);
#endif
			}
		}
#if STRONG_CHECK_SANITY
//		if (total_sent_cnt_ofs[i] != local_wr_cnt) {
//			MWCPUPRINTK("total_sent_cnt_ofs[i] %d != local_wr_cnt %d",
//							total_sent_cnt_ofs[i], local_wr_cnt);
//			BUG_ON(total_sent_cnt_ofs[i] != local_wr_cnt);
//		}
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

		for (i = 0; i < other_node_cpus; i++) {
			struct mw_work *mw_w;
			if (!pgs_to_send[i]) continue;
#if STRONG_CHECK_SANITY
			BUG_ON(pgs_to_send[i] < 0);
#endif

			mw_w = kmalloc(sizeof(*mw_w), GFP_KERNEL);

			mw_w->my_work = NULL;
			// each one has it's offset
			mw_w->rc = rc;
			mw_w->ws = ws;

			mw_w->nid = nid;
			mw_w->total_iter = total_iter; /* dry run required */
			mw_w->local_wr_cnt = local_wr_cnt;
			mw_w->other_node_cpus = other_node_cpus;
			mw_w->pid = current->pid; // always the leader
			mw_w->tso_fence_cnt = current->tso_fence_cnt; // TOD

			// mw_w->tid for dbg

			/* NEW */
			mw_w->per_inv_batch_max = per_inv_batch_max;
			mw_w->pgs_to_send = pgs_to_send[i];
			if (i) { /* NEW */
//			mw_w->iter = i; // TODO; starts from 1 and should not conflict
				//mw_w->iter = iter_ofs[i-1]; // = req_base // = iters I will handle
				mw_w->iter = iter_ofs[i]; // = req_base // = iters I will handle
				mw_w->send_ofs = total_sent_cnt_ofs[i-1]; // send cnt ofs
			} else {
				mw_w->iter = 1; // start from 1
				mw_w->send_ofs = 0;
			}

#if STRONG_CHECK_SANITY
			{
				int handle_reqs;
				if (i) {
					handle_reqs = iter_ofs[i] - iter_ofs[i-1];
				} else {
					handle_reqs = iter_ofs[1] - 1; // req is from 1
				}
				MWCPUPRINTK("wq[%d] - iter base %d send pg base %d "
							"handles %d reqs\n",
							i, mw_w->iter, mw_w->send_ofs, handle_reqs);
			}
#endif

			INIT_WORK(&mw_w->work, mw_parallel_func);
			BUG_ON(!queue_work(mw_wq, &mw_w->work)); // shared starting addr
		}

	} else { BUG(); }

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

extern int do_locally_inv_page(struct task_struct *tsk, struct mm_struct *mm, unsigned long addr);
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
	page_diff_apply_request_t *req = pcn_kmsg_get(sizeof(*req)); // testing
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
#if STRONG_CHECK_SANITY & !MW_IDEAL
	for (i = 0; i < PAGE_SIZE; i++)
		*(req->diff_page + i) = 0;
#endif

	down_read(&tsk->mm->mmap_sem); // TODO: post-icdcs removing locks

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
#if !MW_IDEAL // can extend
	__make_diff(paddr, rcsi_w->paddr, req);
#endif
#else
//	__make_diff(paddr,  rc->inv_pages + (local_ofs * PAGE_SIZE), req);
// TODO remove rc->inv_pages
#endif

#if CONSERVATIVE
	kunmap(page);
	put_page(page);
#else
	kunmap_atomic(paddr);
#endif

	pte_unmap(pte);
	up_read(&tsk->mm->mmap_sem); // TODO: post-icdcs removing locks

#if STRONG_CHECK_SANITY && !MW_IDEAL
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

	/* order issue has been handled by __wait_diffs_done() */
    pcn_kmsg_post(PCN_KMSG_TYPE_PAGE_DIFF_APPLY_REQUEST,
#if !MW_IDEAL
								nid, req, sizeof(*req));
#else // MW_IDEAL
								nid, req, sizeof(*req) - PAGE_SIZE);
#endif
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

	down_read(&tsk->mm->mmap_sem); // TODO: post-icdcs removing locks

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

#if !MW_IDEAL // can cover more area
	__apply_diff(paddr, req->diff_page);
#endif

#if CONSERVATIVE
	kunmap(page);
	put_page(page);
#else
	kunmap_atomic(paddr);
#endif

	pte_unmap(pte);
	up_read(&tsk->mm->mmap_sem); // TODO: post-icdcs removing locks
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
#if MW_TIME
	atomic_inc(&rc->sys_ww_cnt);
#endif
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
		bool conflict = false;
#if HASH_GLOBAL
		/* !lock - concurrent read only */
		struct rcsi_work *rcsi_w = __rcsi_hash(req_addr);
		if (rcsi_w)
			conflict = true;
#else // remove
//		for (j = 0; j < rc->inv_cnt; j++) {
//			unsigned long addr = rc->inv_addrs[j];
//			if (req_addr == addr) {
//				conflict = true;
//				break;
//			}
//		}
#endif //remove

		if (!req_addr) {
			printk(KERN_ERR "\t\t\t[ar/%d/%3d] %3s 0x%lx\n",
								my_nid, i, "inv", req_addr);
			BUG_ON(!req_addr);
		}

		if (!conflict) {
			/* optimize: delay */
			SYNCPRINTK3("\t\t\t[ar/%d/%3d] %3s 0x%lx\n", my_nid, i, "inv", req_addr);
			do_locally_inv_page(tsk, tsk->mm, req_addr);
		} else { /* both will detect the same conflict addrs */
			conflict_cnt++; /* maintain meta to sync */
			SYNCPRINTK3("\t\t\t[ar/%d/%3d] %3s 0x%lx\n", my_nid, i, "ww", req_addr);
			if (my_nid != NOCOPY_NODE) {
				/* optimize: perf: batch */
				do_locally_inv_page(tsk, tsk->mm, req_addr);
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
#if MW_TIME
	rc->remote_sys_ww_cnt += conflict_cnt; /* ww_cnt is a bad name */
#endif
	atomic_add(conflict_cnt, &rc->diffs);
	smp_wmb();
	// only work at origin
	//printk("test id %d\n", tsk->tso_region_id);
#if SMART_REGION_DBG
	if (tsk->tso_region_id) {
		struct omp_region *region = __omp_region_hash(rc, tsk->tso_region_id);
		if (region) // only work for NOCOPY = 0
			region->in_region_conflict_sum += conflict_cnt;
	}
#endif
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
#if MW_TIME
	ktime_t mw_process_req_wait_last_done = ktime_get();
	ktime_t mw_process_req_wait_local_list_ready;
	ktime_t mw_process_req_find_collision_at_remote;
#endif

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

#if MW_TIME
	mw_process_req_wait_local_list_ready = ktime_get();
#endif

	__wait_local_list_ready(rc, wr_cnt);


#if MW_TIME
	mw_process_req_find_collision_at_remote = ktime_get();
#endif
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

#if MW_TIME
	{
		ktime_t pf_end = ktime_get();

		atomic64_add(ktime_to_ns(ktime_sub(mw_process_req_wait_local_list_ready, mw_process_req_wait_last_done)),
								&mw_wait_last_done);
		atomic64_add(ktime_to_ns(ktime_sub(mw_process_req_find_collision_at_remote, mw_process_req_wait_local_list_ready)),
										&mw_wait_local_list_ready);
		atomic64_add(ktime_to_ns(ktime_sub(pf_end, mw_process_req_find_collision_at_remote)),
				&mw_find_collision_at_remote);
	}
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
	BUG_ON(rc->threads_cnt > MAX_POPCORN_THREADS);
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

//	if (!rc->tso_end_m_cnt && current->tso_end_m_cnt)
//		rc->tso_end_m_cnt = current->tso_end_m_cnt;
//	if (rc->tso_end_m_cnt && current->tso_end_m_cnt)
//		BUG_ON(rc->tso_end_m_cnt != current->tso_end_m_cnt);

	/* raw I put */
	if (!rc->kmpc_dispatch_init && current->kmpc_dispatch_init)
		rc->kmpc_dispatch_init = current->kmpc_dispatch_init;
	if (rc->kmpc_dispatch_init && current->kmpc_dispatch_init)
		BUG_ON(rc->kmpc_dispatch_init != current->kmpc_dispatch_init);

	if (!rc->kmpc_static_init && current->kmpc_static_init)
		rc->kmpc_static_init = current->kmpc_static_init;
	if (rc->kmpc_static_init && current->kmpc_static_init)
		BUG_ON(rc->kmpc_static_init != current->kmpc_static_init);

	// popcorn_tso_fence = kmpc_barrier_cnt + kmpc_cancel_barrier_cnt
	if (!rc->kmpc_barrier_cnt && current->kmpc_barrier_cnt)
		rc->kmpc_barrier_cnt = current->kmpc_barrier_cnt;
	if (rc->kmpc_barrier_cnt && current->kmpc_barrier_cnt)
		BUG_ON(rc->kmpc_barrier_cnt != current->kmpc_barrier_cnt);
	if (!rc->kmpc_cancel_barrier_cnt && current->kmpc_cancel_barrier_cnt)
		rc->kmpc_cancel_barrier_cnt = current->kmpc_cancel_barrier_cnt;
	if (rc->kmpc_cancel_barrier_cnt && current->kmpc_cancel_barrier_cnt)
		BUG_ON(rc->kmpc_cancel_barrier_cnt != current->kmpc_cancel_barrier_cnt);

	if (!rc->dispatch_next_to_static_init && current->dispatch_next_to_static_init)
		rc->dispatch_next_to_static_init = current->dispatch_next_to_static_init;
	if (rc->dispatch_next_to_static_init && current->dispatch_next_to_static_init)
		if (rc->dispatch_next_to_static_init !=
				current->dispatch_next_to_static_init)
			printk("rc->dispatch_next_to_static_init %lu != "
						"current->dispatch_next_to_static_init %lu",
									rc->dispatch_next_to_static_init,
									current->dispatch_next_to_static_init);

	if (!rc->static_init_to_static_skewed_init && current->static_init_to_static_skewed_init)
		rc->static_init_to_static_skewed_init = current->static_init_to_static_skewed_init;
	if (rc->static_init_to_static_skewed_init && current->static_init_to_static_skewed_init)
		if (rc->static_init_to_static_skewed_init !=
				current->static_init_to_static_skewed_init)
			printk("rc->static_init_to_static_skewed_init %lu != "
						"current->static_init_to_static_skewed_init %lu",
									rc->static_init_to_static_skewed_init,
									current->static_init_to_static_skewed_init);

	if (!rc->static_skewed_init && current->static_skewed_init)
		rc->static_skewed_init = current->static_skewed_init;
	if (rc->static_skewed_init && current->static_skewed_init)
		if (rc->static_skewed_init != current->static_skewed_init)
			printk("rc->static_skewed_init %lu != "
						"current->static_skewed_init %lu",
									rc->static_skewed_init,
									current->static_skewed_init);

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

	if (!rc->kmpc_static_fini && current->kmpc_static_fini)
		rc->kmpc_static_fini = current->kmpc_static_fini;
	if (rc->kmpc_static_fini && current->kmpc_static_fini)
		BUG_ON(rc->kmpc_static_fini != current->kmpc_static_fini);


#if 0 // hhb hhcb hhbf in
//	if (!rc->hhb && current->hhb)
//		rc->hhb = current->hhb;
#endif
//	if (!rc-> && current->)
//		rc-> = current->;

#if OUTSIDE_REGION_DEBUG
#if 0
	if (current->inside_region_time) {
		printk("[%d] inside region time %lu s %lu ns\n",
					current->pid,
					current->inside_region_time/1000/1000/1000,
					current->inside_region_time);
		if (current->inside_region_time > longest_inside_region_time)
			longest_inside_region_time = current->inside_region_time;
		if (shortest_inside_region_time == 0)
			shortest_inside_region_time = current->inside_region_time;
		if (current->inside_region_time < shortest_inside_region_time)
			shortest_inside_region_time = current->inside_region_time;
	}
#endif
#endif

	__put_task_remote(rc);
}

#include "../../msg_layer/ring_buffer.h"
void collect_tso_wr(struct task_struct *tsk)
{
#if STATIS_END // put for all variables
#ifdef CONFIG_X86_64
	long int divider = X86_THREADS;
#else
	long int divider = ARM_THREADS;
#endif

	struct remote_context *rc = get_task_remote(tsk);
	printk("[%d]: %s (inv) all [[%lu]] = "
				"ww %d (remote ww [[%lu]]) + inv [[%lu]] "
				"smart_skip_cnt %ld (how many VAIN regions skipped)\n",
//				"smart_region_skip %d\n",
						tsk->pid, tsk->comm, rc->sys_rw_cnt,
						atomic_read(&rc->sys_ww_cnt),
						rc->remote_sys_ww_cnt,			// ww
						rc->sys_rw_cnt - rc->remote_sys_ww_cnt, // inv
//						atomic_read(&rc->sys_smart_skip_cnt));
						smart_skip_cnt / divider);
						/* ww_cnt is a bad name */
						//"violation_begin %d violation_end %d "
						//violation_begin, violation_end,
//#if REGION_CHECK
	printk("(violate) violation_begin %d violation_end %d\n",
							violation_begin, violation_end);

						// !current->tso_region count for invain (pgs)

	printk("==============================\n");
	printk("=== MSG & other parameters ===\n");
	printk("==============================\n");
	printk("- MSG_POOL_SIZE = %d\n", MSG_POOL_SIZE);
	//printk("testing: increase rb size. RB_NR_CHUNKS 16\n");
	printk("TODO: test: increase rb size. current RB_NR_CHUNKS = %d\n", RB_NR_CHUNKS);
	printk("- max msg: %lu KB\n", (PCN_KMSG_MAX_SIZE >> 10));

	printk("- MAX_READ_BUFFERS %d\n", MAX_READ_BUFFERS);
	printk("- pf[%d] = MAX_READ_BUFFERS * MAX_POPCORN_THREADS\n",
						MAX_READ_BUFFERS * MAX_POPCORN_THREADS);
	printk("- MAX_WRITE_NOPAGE_BUFFERS %d\n", MAX_WRITE_NOPAGE_BUFFERS);
	//printk(" %ld\n", );

	printk("- divider = %ld (my cpu cnt)\n", divider);
	printk("- MAX_POPCORN_THREADS %d = max(X86_T, ARM_T);\n", MAX_POPCORN_THREADS);

	printk("- FAULTS_HASH = %d\n", FAULTS_HASH);
	printk("- STRONG_CHECK_SANITY (%s)\n", STRONG_CHECK_SANITY ? "O" : "X");

	printk("- MAX_OMP_REGIONS %d\n", MAX_OMP_REGIONS);
	printk("- OMP_REGION_HASH_BITS %d in rc\n", OMP_REGION_HASH_BITS);
	//printk(" (%s)\n",  ? "O" : "X");
	printk("- struct omp_region = %lu B = %lu pgs\n",
						sizeof(struct omp_region),
						sizeof(struct omp_region) / PAGE_SIZE);
	printk("- struct sys_omp_region = %lu B = %lu pgs\n",
						sizeof(struct sys_omp_region),
						sizeof(struct sys_omp_region) / PAGE_SIZE);
	printk("TODO: \"cat /proc/interrupts\" "
			"WQ is per thread -> check IRQ if same core "
					"-> WQ doese auto balance? (checking)\n");
	printk("\n");

	printk("===============\n");
	printk("===== tso =====\n");
	printk("===============\n");
	// __popcorn_tso_end
	printk("(my)(Iput)(actually_perform)(__kmpc_dispatch_init tso_begin) "
			"begin %lu fence %lu end %lu(always 0)\n",
			rc->tso_begin_cnt, rc->tso_fence_cnt, rc->tso_end_cnt);
	printk("(raw) kmpc_dispatch_init [[%lu]] kmpc_dispatch_fini [[%lu]]\n",
						rc->kmpc_dispatch_init, rc->kmpc_dispatch_fini);
	printk("(raw) kmpc_static_init [[%lu]] kmpc_static_fini [[%lu]]\n",
						rc->kmpc_static_init, rc->kmpc_static_fini);
	printk("(raw) hhcb %lu <= (using) kmpc_cancel_barrier [[%lu]]\n",
							rc->hhcb, rc->kmpc_cancel_barrier_cnt);

	printk("\t\t hhb %lu <= (using) __kmpc_barrier [[%lu]] <= "
							"kmpc_reduce %lu + kmpc_end_reduce %lu\n",
							rc->hhb, rc->kmpc_barrier_cnt,
							rc->kmpc_reduce, rc->kmpc_end_reduce);

	 printk("(my_Iput)(merge_phase=fence=)(popcorn_tso_fence) = "
			"kmpc_barrier_cnt (%lu) + kmpc_cancel_barrier (%lu)\n",
				rc->kmpc_barrier_cnt, rc->kmpc_cancel_barrier_cnt);

	printk("(raw)(next merge barrier) hhb %lu hhcb %lu hhbf %lu (TODO)\n",
									rc->hhb, rc->hhcb, rc->hhbf);

	printk("\n\n");
	printk("(raw)(__kmpc_for_static_init) begin_m %lu "
			"((not moved) Barriers)fence_m %lu\n",
			rc->tso_begin_m_cnt, rc->tso_fence_m_cnt);
			//"(__kmpc_for_static_fini) end_m %lu\n",
			//rc->tso_begin_m_cnt, rc->tso_fence_m_cnt, rc->tso_end_m_cnt);

	//if (rc->tso_begin_m_cnt || rc->tso_fence_m_cnt || rc->tso_end_m_cnt)
	if (rc->tso_begin_m_cnt || rc->tso_fence_m_cnt)
		printk("\ttso: WARNNING - MISSING TO HANDLE SOME REGIONS !!!!!\n");
	else
		printk("\ttso: handled all regions NICELY!\n");

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

	current->smart_is_vain = false;

#if TSO_LOG_TIME
	if (a == KMPC_STATIC_INIT)
		current->kmpc_static_init++;
	else if (a == KMPC_DISPATCH_INIT)
		current->kmpc_dispatch_init++;
#endif

#if OUTSIDE_REGION_DEBUG
#if 0
	if (track_outside_region) {
		WARN_ON_ONCE(current->inside_region_start.tv64);
		current->inside_region_start = ktime_get();
	}
#endif
#endif


#if REGION_CHECK
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
#endif

#if PF_TIME
	if (current->tso_region)
		violation_begin++;
#endif

	region = __omp_region_hash(rc, omp_hash); /* key to struct_region */
	if(region) {
		__region_total_cnt_inc(region);
		__parse_save_region_info(region, file); // debug

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
				PFPRINTK("#omp_hash 0x%lx cnt %d\n", omp_hash, region->cnt);
				//printk("#omp_hash 0x%lx cnt %d\n", omp_hash, region->cnt);
				sys_region = __god_view_prefetch(rc, omp_hash, region->cnt);
				if (!sys_region) {
					write_lock(&sys_region_hash_lock);
					__ondemand_si_region_create(omp_hash, region->cnt);
					write_unlock(&sys_region_hash_lock);
				}

				////////////////////////////////////////////
				// region cnt is specific for pf
				region->cnt++; /* region cnt only used by pf */
				// TODO testing moved to here
				// increase for next (very prograssive)
				////////////////////////////////////////////
			}
			__end_begin_barrier(leader);

			current->omp_hash = omp_hash;
			////////////////////////////////////////////
			// testing in case future current-> region_cnt still need to use this number to match (casued by progressive region->cnt++)
			//current->region_cnt = region->cnt;
			current->region_cnt = region->cnt - 1; /* region cnt only used by pf */
			////////////////////////////////////////////
#if PF_TIME
			if (leader) {
				ktime_t dt, pf_end = ktime_get();
				dt = ktime_sub(pf_end, pf_start);
				BUG_ON(ktime_to_ns(dt) > (unsigned long)((unsigned long)10 *
												(unsigned long)NANOSECOND));
				pf_time += ktime_to_ns(dt);
			}
#endif
		}
#endif


#if SMART_REGION
		/* don't affect prefetch */
		if (region->type & RCSI_VAIN) { /* smart wr */
			SMRPRINTK("[%d] %s(): omp_hash 0x%lx b_cnt %lu bm_cnt %lu "
						"no benefit **SKIP**\n", current->pid, __func__,
						omp_hash, current->tso_begin_cnt, current->tso_begin_m_cnt);
			__region_skip_cnt_inc(region);
			current->smart_is_vain = true;
			return 0;
		}
#endif
	}

	/* TODO: THINK HARD: Doing readfault related works here
		is costly since it has to stop all threads */

	current->tso_region = true;
#if TSO_LOG_TIME
	current->tso_begin_cnt++;
#endif
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
	if(is_skipped_region(current)) {
#if TRY_TO_FLIP_SMART_REGION
		__try_flip_region_type(rc);
#endif
#if MW_TIME
		// !current->tso_region count for invain (pgs)
		atomic_add(current->skip_wr_per_rw_cnt, &rc->sys_smart_skip_cnt);
		//current->smart_skip_cnt += current->skip_wr_per_rw_cnt;
		current->skip_wr_per_rw_cnt = 0;
#endif
	}
#endif

// TODO try to remove __popcorn_tso_fence from end 12/12  remove start
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
// TODO try to remove __popcorn_tso_fence from end 12/12  remove done

#if POPCORN_TSO_BARRIER
	leader = __popcorn_start_begin_barrier(rc, id);
	if (leader) {
#if MW_TIME
		mw_start = ktime_get();
#endif
		/* Main MW protocol time */
		//if (current->tso_region){printk("MERGE wait\n");}
		//__locally_find_conflictions(TO_THE_OTHER_NID(), rc);
		__locally_find_conflictions2(TO_THE_OTHER_NID(), rc);
		//if (current->tso_region){printk("MERGE passed\n");}
#if MW_TIME
		mw_find_conf = ktime_get();
#endif
#if GOD_VIEW
		// dbg - before region_cnt++ just in case
		// This dbg will be skipped by smart region
		__dbg_si_addrs(current->omp_hash, current->region_cnt);
#endif

		/* if never been here, create. todo - see if do outside leader (may be seperate analyze here & create outside). pf need the resgion info */
		region = __create_analyze_region(rc);
		////////////////////////////////////////////
		////////////////////////////////////////////
		//region->cnt++; // TODO testing moving to begin
		////////////////////////////////////////////
		////////////////////////////////////////////
		current->smart_is_vain = false;
		////////////////////////////////////////////
		////////////////////////////////////////////

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
#if TSO_LOG_TIME
	current->tso_fence_cnt++;
#endif
out:
	current->read_cnt = 0;
	current->writenopg_cnt = 0;
	return 0;
}

static int __popcorn_tso_end(int id, void __user * file, unsigned long omp_hash, int a, void __user * b)
{
	RGNPRINTK("[%d] %s(): id %d omp_hash 0x%lx -> implicit fence\n", current->pid, __func__, id, omp_hash);


	/* reduce end */
	// if (a == KMPC_REDUCE) { //01/16/19
	if (a == KMPC_REDUCE_NOWAIT) {
		current->kmpc_reduce_nowait++;
		goto valid_end;
	}

//#if TSO_LOG_TIME
	else if (a == KMPC_REDUCE) {
		current->kmpc_reduce++;
		goto valid_end;
	} else if (a == KMPC_END_REDUCE) {
		current->kmpc_end_reduce++;
		goto valid_end;
		//TODO try to skip
	} else if (a == KMPC_DISPATCH_FINI) {
		current->kmpc_dispatch_fini++;
	} else if ( a == KMPC_STATIC_FINI) {
		current->kmpc_static_fini++;
	}
//#endif

//	if (a == KMPC_REDUCE) {
//		goto valid_end;
//	} else if (a == KMPC_END_REDUCE) {
//		goto valid_end;
//	}

#if TSO_LOG_TIME
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
#endif

#if PF_TIME
	if (!current->tso_region)
		violation_end++;
#endif

	if (!current->tso_region) return 0;
    trace_tso(my_nid, current->pid, id, 'e');
// TODO try to merge user space barrier with kernel space barrier
// TODO rely on compiler auto add barrier latter on which has
//  leader thread waiting in syscall
//  followers TODO also have to call syscall
//  		all try to do __popcorn_tso_fence() plus global_barrier()
//
// TODO try to remove __popcorn_tso_fence from end 12/12 remove start
// TODO

// 11/12 testing
#if POST_ICDCS19_TESTING
	//__popcorn_tso_fence(id, file, omp_hash, a, b);
#else
	__popcorn_tso_fence(id, file, omp_hash, a, b);
#endif

// TODO
// TODO try to remove __popcorn_tso_fence from end 12/12 remove done
#if TSO_LOG_TIME
	current->tso_end_cnt++;
#endif
	current->tso_region = false;
	return 0;

valid_end:
	if (current->tso_region) {
		PCNPRINTK_ERR("ERROR: I dont expect REDUCETION will "
							"be still inside region......."
							"enforce tso_region=false\n");
		current->tso_region = false;
	}
#if OUTSIDE_REGION_DEBUG
#if 0
	if (track_outside_region) {
		WARN_ON_ONCE(!current->inside_region_start.tv64);
		WARN_ON_ONCE(current->inside_region_end.tv64);
		ktime_t dt, region_end = ktime_get();
		dt = ktime_sub(region_end, current->inside_region_start);
		current->inside_region_time += ktime_to_ns(dt);

		current->inside_region_start.tv64 = 0;
		current->inside_region_end.tv64 = 0;
	}
#endif
#endif

	return 0;
}


/*
 * Syscalls
 */
#ifdef CONFIG_POPCORN
SYSCALL_DEFINE5(popcorn_tso_begin, int, id, void __user *, file, unsigned long, omp_hash, int, a, void __user *, b)
{
#if TSO_LOG_TIME
	show_omp_hash(omp_hash);
#endif
	return __popcorn_tso_begin(id, file, omp_hash, a, b);
}

SYSCALL_DEFINE5(popcorn_tso_fence, int, id, void __user *, file, unsigned long, omp_hash, int, a, void __user *, b)
{
#if TSO_LOG_TIME
	show_omp_hash(omp_hash);
	RGNPRINTK("[%d] %s(): id %d omp_hash 0x%lx\n", current->pid, __func__, id, omp_hash);

	if (a == KMPC_BARRIER) {
		current->kmpc_barrier_cnt++;
	} else if (a == KMPC_CANCEL_BARRIER) {
		current->kmpc_cancel_barrier_cnt++;
	}

    trace_tso(my_nid, current->pid, id, 'f');
#endif
	return __popcorn_tso_fence(id, file, omp_hash, id, b);
}

SYSCALL_DEFINE5(popcorn_tso_end, int, id, void __user *, file, unsigned long, omp_hash, int, a, void __user *, b)
{
#if TSO_LOG_TIME
	show_omp_hash(omp_hash);
#endif
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
#if TSO_LOG_TIME
	show_omp_hash(omp_hash);
	current->tso_begin_m_cnt++;
#endif
	//printk("[%d] %s():\n", current->pid, __func__);
	//return __popcorn_tso_begin(id, file, omp_hash, a, b);
	return 0;
}

SYSCALL_DEFINE5(popcorn_tso_fence_manual, int, id, void __user *, file, unsigned long, omp_hash, int, a, void __user *, b)
{
//#if TSO_LOG_TIME
	show_omp_hash(omp_hash);
	RGNPRINTK("[%d] %s():\n", current->pid, __func__);

	if (a == HHB) {
		current->tso_fence_m_cnt++;
		current->mm->remote->hhb++;
	} else if (a == HHCB) {
		current->tso_fence_m_cnt++;
		current->mm->remote->hhcb++;
	} else if (a == HHBF) {
		current->tso_fence_m_cnt++;
		current->mm->remote->hhbf++;
	}
	else if (a == DISPATCH_NEXT_TO_STATIC_INIT) {
		current->dispatch_next_to_static_init++;
	} else if (a == STATIC_INIT_TO_STATIC_SKEWED_INIT) {
		current->static_init_to_static_skewed_init++;
	} else if (a == STATIC_SKEWED_INIT) {
		current->static_skewed_init++;
	}
	else if (a == FORCE_PRINTK_EXEC_TIME) {
		//printk("kM/BLK: Completed %.6lf\n\n", omp_hash);
		printk("\n"
				"==========================================\n"
				"KM/BLK: Completed %lu is_god(%s - 1: record 0: pf ) "
				"pf_cnt %ld(TODO: don't trust)\n"
				"========================================\n\n",
								omp_hash, is_god ? "O" : "X",
									atomic64_read(&fp_cnt));
	}

//	if (a == TOGGLE_MEMORY_PATTERN_TRACE) {
//	}
//#endif

#if OUTSIDE_REGION_DEBUG
	{
		volatile bool toggle = false;
		if (a == OUTSIDE_REGION_START) {
#if 0
			// serial
			WARN_ON_ONCE(track_outside_region);
			WARN_ON_ONCE(first_outside_region.tv64);
			track_outside_region = true;
			printk("[%d] track_outside_region start\n", current->pid);

			first_outside_region = ktime_get();
#endif
			toggle = true;
		} else if (a == OUTSIDE_REGION_END) {
#if 0
			// serial
			WARN_ON_ONCE(!track_outside_region);
			WARN_ON_ONCE(last_outside_region.tv64);
			track_outside_region = false;

			last_outside_region = ktime_get();
			printk("[%d] track_outside_region end\n", current->pid);

			first_outside_region.tv64 = 0;
			last_outside_region.tv64 = 0;
#endif
			toggle = true;
		}

		if (toggle)	{
			toggle_memory_pattern_trace_request_t *req =
								kmalloc(sizeof(*req), GFP_KERNEL);
			struct wait_station *ws = get_wait_station(current);
			BUG_ON(!req);
			req->origin_ws = ws->id;

			is_trace_memory_pattern = !is_trace_memory_pattern;
			printk("(origin) is_trace_memory_pattern = %s\n",
						is_trace_memory_pattern ? "O" : "X");

			BUG_ON(my_nid != 0);
			pcn_kmsg_send(PCN_KMSG_TYPE_TOGGLE_MEMORY_PATTERN_TRACE_REQUEST,
														1, req, sizeof(*req));
			wait_at_station(ws);
			kfree(req);
			printk("(origin) woken up from mem trace req\n");
		}
	}
#endif

	//printk("[%d] %s():\n", current->pid, __func__);
	//return __popcorn_tso_fence_manual(id, file, omp_hash, a, b);
	return 0;
}

SYSCALL_DEFINE5(popcorn_tso_end_manual, int, id, void __user *, file, unsigned long, omp_hash, int, a, void __user *, b)
{
#if TSO_LOG_TIME
	show_omp_hash(omp_hash);
//	current->tso_end_m_cnt++;
#endif
	//printk("[%d] %s():\n", current->pid, __func__);
	//printk("[%d] %s():\n", current->pid, __func__);
	//return __popcorn_tso_end(id, file, omp_hash, a, b);
	return 0;
}

//static int handle_toggle_memory_pattern_trace_request(struct pcn_kmsg_message *msg){
static void process_toggle_memory_pattern_trace_request(struct work_struct *work)
{
	//toggle_memory_pattern_trace_request_t *req =
	//							(toggle_memory_pattern_trace_request_t *)msg;
    START_KMSG_WORK(toggle_memory_pattern_trace_request_t, req, work);
	toggle_memory_pattern_trace_response_t *res =
								kmalloc(sizeof(*res), GFP_KERNEL);
	BUG_ON(!res);
	BUG_ON(my_nid == 0);

	is_trace_memory_pattern = !is_trace_memory_pattern;
	printk("(remote) is_trace_memory_pattern = %s\n",
				is_trace_memory_pattern ? "O" : "X");

	res->origin_ws = req->origin_ws;

	pcn_kmsg_send(PCN_KMSG_TYPE_TOGGLE_MEMORY_PATTERN_TRACE_RESPONSE,
												0, res, sizeof(*res));
	kfree(res);

    END_KMSG_WORK(req);
	//pcn_kmsg_done(req);
	//return 0;
}

//static int handle_toggle_memory_pattern_trace_response(struct pcn_kmsg_message *msg)
static void process_toggle_memory_pattern_trace_response(struct work_struct *work)
{
    //toggle_memory_pattern_trace_response_t *res =
	//			(toggle_memory_pattern_trace_response_t *)msg;
    START_KMSG_WORK(toggle_memory_pattern_trace_response_t, res, work);
    struct wait_station *ws = wait_station(res->origin_ws);
	complete(&ws->pendings);

	printk("(origin) toggle memory trace ack\n");

    END_KMSG_WORK(res);
	//pcn_kmsg_done(res);
	//return 0;
}


/* Popcorn global barrier */
static int handle_global_barrier_request(struct pcn_kmsg_message *msg)
{
//	unsigned long flags;


	// seems line this update confliction will happen
	// pretend from reentrying
	// reader frendly design (no atomic)
	atomic64_inc(&popcorn_global_remote_cnt); // racecondition?

//	//spin_lock(&popcorn_global_lock);
//	spin_lock_irqsave(&popcorn_global_lock, flags); // try
//	popcorn_global_remote_cnt++; // racecondition?
//	smp_wmb(); // look no need
//	spin_unlock_irqrestore(&popcorn_global_lock, flags);
//	//spin_unlock(&popcorn_global_lock);

#if VM_TESTING
////	//printk("\t\t recv remote %lu\n", popcorn_global_remote_cnt);
//	printk("\t\t recv remote %ld\n", atomic64_read(&popcorn_global_remote_cnt));
#endif

//    remote_popcorn_barrier_request_t *req =
//				(remote_popcorn_barrier_request_t *)msg;
	pcn_kmsg_done(msg);
	return 0;
}

SYSCALL_DEFINE5(popcorn_global_barrier, unsigned long, id, void __user *, file, unsigned long, omp_hash, int, a, void __user *, b)
{
#if 0
	u64 ppp = 0;
#endif
#if GLOBAL_SYS_BAR_TIME
	ktime_t start;
#endif
//	unsigned long __popcorn_global_local_cnt;
	remote_popcorn_barrier_request_t *req = pcn_kmsg_get(sizeof(*req));
    pcn_kmsg_post(PCN_KMSG_TYPE_GLOBAL_BARRIER_REQUEST,
					TO_THE_OTHER_NID(), req, sizeof(*req));

//	spin_lock(&popcorn_global_lock2);
//	__popcorn_global_local_cnt = ++popcorn_global_local_cnt;
//	smp_wmb();
//	spin_unlock(&popcorn_global_lock2);

	atomic64_inc(&popcorn_global_local_cnt);
#if VM_TESTING
//	printk("[%d] coming local (sys) %lu(sent)\n", current->pid,
//							 atomic64_read(&popcorn_global_local_cnt));
//////////									 __popcorn_global_local_cnt);
#endif
//	static u64 cnt = 0;
//	if (cnt++ < 100)
//		printk("TODO: IMPLEMENTING\n");

#if GLOBAL_SYS_BAR_TIME
	start = ktime_get();
#endif
	while (atomic64_read(&popcorn_global_local_cnt) >
			atomic64_read(&popcorn_global_remote_cnt)) {
//	while (__popcorn_global_local_cnt >
//			popcorn_global_remote_cnt) {
		//;
//#if VM_TESTING
#if 0
		if (ppp++ > 100000000000 && ppp < (100000000000 + 100)) {
			printk("[%d]WAIT TOO LONG "
						"local (sys) %lu(sent) remote %lu\n",
									current->pid,
									__popcorn_global_local_cnt,
									//atomic64_read(&popcorn_global_local_cnt),
									//atomic64_read(&popcorn_global_remote_cnt));
									popcorn_global_remote_cnt);
		}
#endif

//		smp_rmb(); // must need
		/* only single CPU I think I can spin */
		CPU_RELAX;
	}
#if GLOBAL_SYS_BAR_TIME
	{
		ktime_t end = ktime_get();
		atomic64_add(ktime_to_ns(ktime_sub(end, start)),
									&popcorn_global_time);
	}
#endif
	return 0;
}
#else // CONFIG_POPCORN
SYSCALL_DEFINE5(popcorn_tso_begin, int, id, void __user *, file, unsigned long, omp_hash, int, a void __user *, b)
{
	PCNPRINTK_ERR("Kernel is not configured to use popcorn\n");
	return -EPERM;
}

SYSCALL_DEFINE5(popcorn_tso_fence, int, id, void __user *, file, unsigned long, omp_hash, int, a, void __user *, b)
{
	PCNPRINTK_ERR("Kernel is not configured to use popcorn\n");
	return -EPERM;
}

SYSCALL_DEFINE5(popcorn_tso_end, int, id, void __user *, file, unsigned long, omp_hash, int, a, void __user *, b)
{
	PCNPRINTK_ERR("Kernel is not configured to use popcorn\n");
	return -EPERM;
}

SYSCALL_DEFINE5(popcorn_tso_id, int, id, void __user *, file, unsigned long, omp_hash, int, a, void __user *, b)
{
	PCNPRINTK_ERR("Kernel is not configured to use popcorn\n");
	return -EPERM;
}


SYSCALL_DEFINE5(popcorn_tso_begin_manual, int, id, void __user *, file, unsigned long, omp_hash, int, a void __user *, b)
{
	PCNPRINTK_ERR("Kernel is not configured to use popcorn\n");
	return -EPERM;
}

SYSCALL_DEFINE5(popcorn_tso_fence_manual, int, id, void __user *, file, unsigned long, omp_hash, int, a, void __user *, b)
{
	PCNPRINTK_ERR("Kernel is not configured to use popcorn\n");
	return -EPERM;
}

SYSCALL_DEFINE5(popcorn_tso_end_manual, int, id, void __user *, file, unsigned long, omp_hash, int, a, void __user *, b)
{
	PCNPRINTK_ERR("Kernel is not configured to use popcorn\n");
	return -EPERM;
}

SYSCALL_DEFINE5(popcorn_global_barrier, unsigned long, id, void __user *, file, unsigned long, omp_hash, int, a void __user *, b)
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

DEFINE_KMSG_WQ_HANDLER(toggle_memory_pattern_trace_request);
DEFINE_KMSG_WQ_HANDLER(toggle_memory_pattern_trace_response);
int __init popcorn_sync_init(void)
{
	//__rcsi_hash_test();
	__rcsi_mem_alloc();

	mw_wq = alloc_workqueue("mw_wq", WQ_MEM_RECLAIM, 0);
	BUG_ON(!mw_wq);

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

	/* popcorn kernel barrier */
    REGISTER_KMSG_HANDLER(
			PCN_KMSG_TYPE_GLOBAL_BARRIER_REQUEST, global_barrier_request);

#if GOD_VIEW
    //REGISTER_KMSG_HANDLER(	/* msg lost somehow */ /* main scatters */
    REGISTER_KMSG_WQ_HANDLER( // sleep
        PCN_KMSG_TYPE_REMOTE_PREFETCH_REQUEST, remote_prefetch_request);
    //REGISTER_KMSG_HANDLER(	/* msg lost somehow */ /* main scatters */
    REGISTER_KMSG_WQ_HANDLER( // fixup /* pg fixup may sleep */ // loose
        PCN_KMSG_TYPE_REMOTE_PREFETCH_RESPONSE, remote_prefetch_response);
#endif

	/* memory access pattern trace */
//    REGISTER_KMSG_HANDLER(
	REGISTER_KMSG_WQ_HANDLER(
		PCN_KMSG_TYPE_TOGGLE_MEMORY_PATTERN_TRACE_REQUEST,
						toggle_memory_pattern_trace_request);
//    REGISTER_KMSG_HANDLER(
    REGISTER_KMSG_WQ_HANDLER(
		PCN_KMSG_TYPE_TOGGLE_MEMORY_PATTERN_TRACE_RESPONSE,
						toggle_memory_pattern_trace_response);

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
		for (i = 0; i < MAX_POPCORN_THREADS; i++) {
			handle_mw_reqest_cpu[i] = 0;
			handle_pf_reqest_cpu[i] = 0;
		}
	}
#endif

	smart_skip_cnt = 0;
	real_sent_pf = 0;

	real_recv_pf_succ = 0;
	real_recv_pf_fail = 0;

	// violation regions
	violation_begin = 0;
	violation_end = 0;

#if DBG_OMP_HASH
	{	int i;
		for (i = 0; i < OMP_HASH_DBG_CNT; i++)
			omp_hash_dbg_arr[i] = 0;
	}
	spin_lock_init(&omp_hash_dbg_lock);
#endif

	spin_lock_init(&popcorn_global_lock);
	spin_lock_init(&popcorn_global_lock2);

	/* larger MAX_MSG_SIZE -> more inv addr -> larger rc size */
	printk("sizeof(*rc) %lu >? (PAGE_SIZE << (MAX_ORDER - 1)) %lu\n",
				sizeof(struct remote_context), (PAGE_SIZE << (MAX_ORDER - 1)));
	WARN_ON(sizeof(struct remote_context) > (PAGE_SIZE << (MAX_ORDER - 1)));
	printk("sizeof(*sys_region) %lu >? (PAGE_SIZE << (MAX_ORDER - 1))\n",
				sizeof(struct sys_omp_region));
	WARN_ON(sizeof(struct sys_omp_region) > (PAGE_SIZE << (MAX_ORDER - 1)));

	printk("MAX_ORDER %d\n", MAX_ORDER);
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
	printk("(malloc MAX size = 1 << MAX_ORDER) * PAGE_SIZE = %lu Bytes\n",
											(1 << MAX_ORDER) * PAGE_SIZE);
	printk("struct sys_omp_region = %lu Bytes = %lu pgs\n",
						sizeof(struct sys_omp_region),
						sizeof(struct sys_omp_region) / PAGE_SIZE);
	if (sizeof(struct sys_omp_region) > (1 << MAX_ORDER) * PAGE_SIZE) {
		printk("check sys_omp_region size fail\n");
		printk("(1 << MAX_ORDER) * PAGE_SIZE %lu > struct sys_omp_region = %lu\n",
				 (1 << MAX_ORDER) * PAGE_SIZE, sizeof(struct sys_omp_region));
		WARN_ON_ONCE("Reduce sys_region size!!\n");
	} else
		printk("check sys_omp_region size pass\n");
	//BUG_ON(sizeof(struct sys_omp_region) > 1 << MAX_ORDER);

	return 0;
}
