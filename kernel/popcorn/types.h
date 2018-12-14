#ifndef __POPCORN_TYPES_H__
#define __POPCORN_TYPES_H__

#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/completion.h>
#include <linux/workqueue.h>
#include <linux/signal.h>
#include <linux/slab.h>
#include <linux/radix-tree.h>

#include <popcorn/pcn_kmsg.h>
#include <popcorn/regset.h>
#include <popcorn/sync.h>
#include <linux/semaphore.h>
#include <linux/hashtable.h>

#define FAULTS_HASH 31
#define GLOBAL 1 /* 1: 1 list for a system 0: 1 list per thread */
// !GLOBAL is note working becasue it has old version code
#define NOCOPY_NODE 0 /* this node doesn't have to generate diff and alway will be the owner if conflicting */

#define HASH_GLOBAL 1

#define GOD_VIEW 1

#define OMP_REGION_HASH_BITS 10

/* Multiple writer protocol -
 * by providing postpone inv (batch invs) and merging in the end
 */
#define MW 1

/* Prefetch */
#define PREFETCH 1
#define MAX_PF_REQ (int)(PCN_KMSG_MAX_PAYLOAD_SIZE / PAGE_SIZE)	// () is so important!!!
#define PREFETCH_FAIL 0x0001
#define PREFETCH_SUCCESS 0x0002

/**
 * OMP region
 */
//#define VAIN_THRESHOLD (PCN_KMSG_MAX_PAYLOAD_SIZE / PAGE_SIZE)
#define VAIN_THRESHOLD 50
//#define VAIN_REGION_REPEAT_THRESHOLD 5 /* consecutive non-benefit regions then skip forever */
//#define BENEFIT_REGION_REPEAT_THRESHOLD 2 /* consecutive non-benefit regions then skip forever */
#define VAIN_REGION_REPEAT_THRESHOLD 10 /* consecutive non-benefit regions then skip forever */
#define BENEFIT_REGION_REPEAT_THRESHOLD 5 /* consecutive non-benefit regions then skip forever */

/* omp_region_type */
#define RCSI_UNKNOW 0x00
#define RCSI_VAIN 0x01
#define RCSI_REPEAT 0x20
#define RCSI_SERIAL 0x40

extern int TO_THE_OTHER_NID(void);

/**
 * Remote execution context
 */
struct remote_context {
	struct list_head list;
	atomic_t count;
	struct mm_struct *mm;

	int tgid;
	bool for_remote;

	/* Tracking page status */
	struct radix_tree_root pages;

	/* For page replication protocol */
	spinlock_t faults_lock[FAULTS_HASH];
	struct hlist_head faults[FAULTS_HASH];

	/* For VMA management */
	spinlock_t vmas_lock;
	struct list_head vmas;

	/* Remote worker */
	bool stop_remote_worker;

	struct task_struct *remote_worker;
	struct completion remote_works_ready;
	spinlock_t remote_works_lock;
	struct list_head remote_works;

	pid_t remote_tgids[MAX_POPCORN_NODES];

	/* Barrier for batch inv sync (for WW case) */
	wait_queue_head_t waits_begin; /* for tso_begin */
	wait_queue_head_t waits_end;
	atomic_t barrier_begin;		/* for tso_begin */
	atomic_t barrier_end;
	atomic_t pendings_begin;	/* for tso_begin */
	atomic_t pendings_end; 		/* for leader and follower race condition */
	atomic_t scatter_pendings; 	/* for the last scatter to wake up leader */

	/* Track live threads */
	unsigned short threads_cnt; /* alive threads */
	//unsigned short migrated; /* -> out_threads */
	int out_threads; // TODO renam. keep tracking current on-node thread cnt
	int pids[MAX_ALIVE_THREADS];
	spinlock_t pids_lock;

	bool ready; /* global list prepared */ /* ready for remote to check the confliction (producer/consumer) */

	/* OMP region hash */
	DECLARE_HASHTABLE(omp_region_hash, OMP_REGION_HASH_BITS);
	rwlock_t omp_region_hash_lock;

#if GLOBAL
	/* Global */
	spinlock_t inv_lock;
	int inv_cnt; /* Per region */
	int remote_fence;
	unsigned long inv_addrs[MAX_ALIVE_THREADS * MAX_WRITE_INV_BUFFERS]; /* for sending */
#if !HASH_GLOBAL
	char *inv_pages; // [MAX_ALIVE_THREADS * MAX_WRITE_INV_BUFFERS][PAGE_SIZE]; // will remove
#endif
	/* Application wide */
	unsigned long sys_rw_cnt;
	atomic_t sys_ww_cnt;
	unsigned long sys_inv_cnt; /* not used */
	atomic_t sys_smart_skip_cnt;

	unsigned long remote_sys_ww_cnt;

	unsigned long sys_local_conflict_cnt; /* more details */
#else
	/* per-thread */ // - TODO check the global time is right
	spinlock_t inv_lock_t[MAX_ALIVE_THREADS];
	int inv_cnt_t[MAX_ALIVE_THREADS];
	char *inv_pages_t[MAX_ALIVE_THREADS]; //[MAX_ALIVE_THREADS][MAX_WRITE_INV_BUFFERS][PAGE_SIZE];
	unsigned long time_t[MAX_ALIVE_THREADS][MAX_WRITE_INV_BUFFERS];

	int lconf_cnt; /* local_conflict_addr_cnt */
	//int inv_cnt; /* local invalidation addr cnt */ // == local_wr_cnt (local variable now)
#endif
	atomic_t diffs;			/* per region + async + concurrent */
	atomic_t per_barrier_reset_done; /* per region + async + concurrent */
	atomic_t req_diffs;		/* per region */
	bool remote_done; /* XXX this has been replaced by remote_done_cnt. REMOVE IT */
	unsigned long remote_type;
	int local_done_cnt;
	int remote_done_cnt;
	int local_merge_id;
	int remote_merge_id;
	/* bool leader back // can cover diffs but conor case? */
	atomic_t doing_diff_cnt;

	// prefetch
	atomic_t pf_ongoing_cnt;            /* Ongoing prefetch cnt XXXXX BUG XXXXX*/
	spinlock_t pf_ongoing_lock;         /* Ongoing prefetch mapping lock */
	struct list_head pf_ongoing_list;   /* Ongoing prefetch mapping list head */
	int wrong_hist;
	int wrong_hist_no_vma;
	// statis
	atomic_t pf_succ_cnt;

	/* for region dbg buffering */
	char name[256];
	int line;
};

struct fault_handle {
	struct hlist_node list;

	unsigned long addr;
	unsigned long flags;

	unsigned int limit;
	pid_t pid;
	int ret;

	atomic_t pendings;
	atomic_t pendings_retry;
	wait_queue_head_t waits;
	wait_queue_head_t waits_retry;
	struct remote_context *rc;

	struct completion *complete;
};

// prefetch
struct prefetch_list_body {
    unsigned long addr;
    //bool is_write;
    //bool is_besteffort;

    //dma_addr_t rdma_addr;   /* redundant */ /*change name to addr*/
    //u32 rdma_key;   /* redundant */
    //struct pcn_kmsg_rdma_handle *pf_handle; /* move to list head*/
}__attribute__((packed)); // try to remove

struct prefetch_result {
    unsigned long addr;
    //bool is_write;
    unsigned long result;
};

struct pf_ongoing_map {
    struct list_head list;
    int pf_list_size;
    int pf_req_id;
    unsigned long addr[MAX_PF_REQ];
    struct fault_handle *fh[MAX_PF_REQ];
    struct pcn_kmsg_rdma_handle *rh[MAX_PF_REQ];
};



struct remote_context *__get_mm_remote(struct mm_struct *mm);
struct remote_context *get_task_remote(struct task_struct *tsk);
bool put_task_remote(struct task_struct *tsk);
bool __put_task_remote(struct remote_context *rc);

/**
 * Process migration
 */
#define BACK_MIGRATION_FIELDS \
	int remote_nid;\
	pid_t remote_pid;\
	pid_t origin_pid;\
	unsigned int personality;\
	/* \
	unsigned long def_flags;\
	sigset_t remote_blocked;\
	sigset_t remote_real_blocked;\
	sigset_t remote_saved_sigmask;\
	struct sigpending remote_pending;\
	unsigned long sas_ss_sp;\
	size_t sas_ss_size;\
	struct k_sigaction action[_NSIG]; \
	*/ \
	struct field_arch arch;
DEFINE_PCN_KMSG(back_migration_request_t, BACK_MIGRATION_FIELDS);

#define CLONE_FIELDS \
	pid_t origin_tgid;\
	pid_t origin_pid;\
	unsigned long task_size; \
	unsigned long stack_start; \
	unsigned long env_start;\
	unsigned long env_end;\
	unsigned long arg_start;\
	unsigned long arg_end;\
	unsigned long start_brk;\
	unsigned long brk;\
	unsigned long start_code ;\
	unsigned long end_code;\
	unsigned long start_data;\
	unsigned long end_data;\
	unsigned int personality;\
	unsigned long def_flags;\
	char exe_path[512];\
	/* \
	sigset_t remote_blocked;\
	sigset_t remote_real_blocked;\
	sigset_t remote_saved_sigmask;\
	struct sigpending remote_pending;\
	unsigned long sas_ss_sp;\
	size_t sas_ss_size;\
	struct k_sigaction action[_NSIG];\
	*/ \
	struct field_arch arch;
DEFINE_PCN_KMSG(clone_request_t, CLONE_FIELDS);


/**
 * This message is sent in response to a clone request.
 * Its purpose is to notify the requesting cpu that make
 * the specified pid is executing on behalf of the
 * requesting cpu.
 */
#define REMOTE_TASK_PAIRING_FIELDS \
	pid_t my_tgid; \
	pid_t my_pid; \
	pid_t your_pid;
DEFINE_PCN_KMSG(remote_task_pairing_t, REMOTE_TASK_PAIRING_FIELDS);


#define REMOTE_TASK_EXIT_FIELDS  \
	pid_t origin_pid; \
	pid_t remote_pid; \
	int exit_code;
DEFINE_PCN_KMSG(remote_task_exit_t, REMOTE_TASK_EXIT_FIELDS);

#define ORIGIN_TASK_EXIT_FIELDS \
	pid_t origin_pid; \
	pid_t remote_pid; \
	int exit_code;
DEFINE_PCN_KMSG(origin_task_exit_t, ORIGIN_TASK_EXIT_FIELDS);


/**
 * VMA management
 */
#define VMA_INFO_REQUEST_FIELDS \
	pid_t origin_pid; \
	pid_t remote_pid; \
	unsigned long addr;
DEFINE_PCN_KMSG(vma_info_request_t, VMA_INFO_REQUEST_FIELDS);

#define VMA_INFO_RESPONSE_FIELDS \
	pid_t remote_pid; \
	int result; \
	unsigned long addr; \
	unsigned long vm_start; \
	unsigned long vm_end; \
	unsigned long vm_flags;	\
	unsigned long vm_pgoff; \
	char vm_file_path[512];
DEFINE_PCN_KMSG(vma_info_response_t, VMA_INFO_RESPONSE_FIELDS);

#define vma_info_anon(x) ((x)->vm_file_path[0] == '\0' ? true : false)


#define VMA_OP_REQUEST_FIELDS \
	pid_t origin_pid; \
	pid_t remote_pid; \
	int remote_ws; \
	int operation; \
	union { \
		unsigned long addr; \
		unsigned long start; \
		unsigned long brk; \
	}; \
	union { \
		unsigned long len;		/* mmap */ \
		unsigned long old_len;	/* mremap */ \
	}; \
	union { \
		unsigned long prot;		/* mmap */ \
		int behavior;			/* madvise */ \
		unsigned long new_len;	/* mremap */ \
	}; \
	unsigned long flags;		/* mmap, remap */ \
	union { \
		unsigned long pgoff;	/* mmap */ \
		unsigned long new_addr;	/* mremap */ \
	}; \
	char path[512];
DEFINE_PCN_KMSG(vma_op_request_t, VMA_OP_REQUEST_FIELDS);

#define VMA_OP_RESPONSE_FIELDS \
	pid_t origin_pid; \
	pid_t remote_pid; \
	int remote_ws; \
	int operation; \
	long ret; \
	union { \
		unsigned long addr; \
		unsigned long start; \
		unsigned long brk; \
	}; \
	unsigned long len;
DEFINE_PCN_KMSG(vma_op_response_t, VMA_OP_RESPONSE_FIELDS);


/**
 * Page management
 */
#define REMOTE_PAGE_REQUEST_FIELDS \
	pid_t origin_pid; \
	int origin_ws; \
	pid_t remote_pid; \
	unsigned long addr; \
	unsigned long fault_flags; \
	unsigned long instr_addr; \
	dma_addr_t rdma_addr; \
	u32 rdma_key;
DEFINE_PCN_KMSG(remote_page_request_t, REMOTE_PAGE_REQUEST_FIELDS);

#define REMOTE_PAGE_RESPONSE_COMMON_FIELDS \
	pid_t remote_pid; \
	pid_t origin_pid; \
	int origin_ws; \
	unsigned long addr; \
	int result;

#define REMOTE_PAGE_RESPONSE_FIELDS \
	REMOTE_PAGE_RESPONSE_COMMON_FIELDS \
	unsigned char page[PAGE_SIZE];
DEFINE_PCN_KMSG(remote_page_response_t, REMOTE_PAGE_RESPONSE_FIELDS);

#define REMOTE_PAGE_GRANT_FIELDS \
	REMOTE_PAGE_RESPONSE_COMMON_FIELDS
DEFINE_PCN_KMSG(remote_page_response_short_t, REMOTE_PAGE_GRANT_FIELDS);

// pf
#define REMOTE_PREFETCH_REQUEST_FIELDS \
    pid_t origin_pid; \
    pid_t remote_pid; \
    int pf_req_id; \
    int pf_list_size; \
    unsigned long god_omp_hash; \
    struct prefetch_list_body pf_reqs[MAX_PF_REQ];
DEFINE_PCN_KMSG(remote_prefetch_request_t, REMOTE_PREFETCH_REQUEST_FIELDS);
//    dma_addr_t rdma_addr;   /* redundant */ /*change name to addr*/
//    u32 rdma_key;   /* redundant */
//    struct pcn_kmsg_rdma_handle *pf_handle; /* move to list head*/

// god_omp_hash is for debuging

#define REMOTE_PREFETCH_RESPONSE_COMMON_FIELDS \
    pid_t origin_pid; \
    pid_t remote_pid; \
    int pf_req_id; \
    unsigned long god_omp_hash; \
    struct prefetch_result pf_results[MAX_PF_REQ];

#define REMOTE_PREFETCH_RESPONSE_FIELDS \
    REMOTE_PREFETCH_RESPONSE_COMMON_FIELDS \
    unsigned char page[MAX_PF_REQ][PAGE_SIZE];
DEFINE_PCN_KMSG(remote_prefetch_response_t, REMOTE_PREFETCH_RESPONSE_FIELDS);

#define REMOTE_PREFETCH_RESPONSE_SHORT_FIELDS \
    REMOTE_PREFETCH_RESPONSE_COMMON_FIELDS
DEFINE_PCN_KMSG(remote_prefetch_response_short_t, REMOTE_PREFETCH_RESPONSE_SHORT_FIELDS);

#define REMOTE_PAGE_FLUSH_COMMON_FIELDS \
	pid_t origin_pid; \
	int remote_nid; \
	pid_t remote_pid; \
	int remote_ws; \
	unsigned long addr; \
	unsigned long flags;

#define REMOTE_PAGE_FLUSH_FIELDS \
	REMOTE_PAGE_FLUSH_COMMON_FIELDS \
	unsigned char page[PAGE_SIZE];
DEFINE_PCN_KMSG(remote_page_flush_t, REMOTE_PAGE_FLUSH_FIELDS);

#define REMOTE_PAGE_RELEASE_FIELDS \
	REMOTE_PAGE_FLUSH_COMMON_FIELDS
DEFINE_PCN_KMSG(remote_page_release_t, REMOTE_PAGE_RELEASE_FIELDS);

#define REMOTE_PAGE_FLUSH_ACK_FIELDS \
	int remote_ws; \
	unsigned long flags;
DEFINE_PCN_KMSG(remote_page_flush_ack_t, REMOTE_PAGE_FLUSH_ACK_FIELDS);


#define PAGE_INVALIDATE_REQUEST_FIELDS \
	pid_t origin_pid; \
	int origin_ws; \
	pid_t remote_pid; \
	unsigned long addr;
DEFINE_PCN_KMSG(page_invalidate_request_t, PAGE_INVALIDATE_REQUEST_FIELDS);

#define PAGE_INVALIDATE_RESPONSE_FIELDS \
	pid_t origin_pid; \
	int origin_ws; \
	pid_t remote_pid;
DEFINE_PCN_KMSG(page_invalidate_response_t, PAGE_INVALIDATE_RESPONSE_FIELDS);

#define PAGE_INVALIDATE_BATCH_REQUEST_FIELDS \
	pid_t origin_pid; \
	int origin_ws; \
	pid_t remote_pid; \
	unsigned long tso_wr_cnt; \
	unsigned long addrs[MAX_WRITE_INV_BUFFERS];
DEFINE_PCN_KMSG(page_invalidate_batch_request_t,
				PAGE_INVALIDATE_BATCH_REQUEST_FIELDS);

#define PAGE_INVALIDATE_BATCH_RESPONSE_FIELDS \
	pid_t origin_pid; \
	int origin_ws; \
	pid_t remote_pid; \
	unsigned long retry_cnt; \
	unsigned long retry_addrs[MAX_WRITE_INV_BUFFERS];
DEFINE_PCN_KMSG(page_invalidate_batch_response_t,
				PAGE_INVALIDATE_BATCH_RESPONSE_FIELDS);

#define PAGE_MERGE_REQUEST_FIELDS \
	pid_t origin_pid; \
	int origin_ws; \
	pid_t remote_pid; \
	int iter; \
	int total_iter; \
	unsigned long fence; \
	int merge_id; \
	unsigned long wr_cnt; \
	unsigned long addrs[MAX_WRITE_INV_BUFFERS];
DEFINE_PCN_KMSG(page_merge_request_t, PAGE_MERGE_REQUEST_FIELDS);

#define PAGE_MERGE_RESPONSE_FIELDS \
	pid_t origin_pid; \
	int origin_ws; \
	pid_t remote_pid; \
	int iter; \
	int total_iter; \
	int merge_id; \
	unsigned long wr_cnt; \
	int conflict_cnt
DEFINE_PCN_KMSG(page_merge_response_t, PAGE_MERGE_RESPONSE_FIELDS);
/* scatters = total_iter = if(0) = no need to merge */
/* merge_id = iter*/
//	char diffs[PAGE_SIZE];

/* more info needed*/
#define PAGE_DIFF_APPLY_REQUEST_FIELDS \
	pid_t origin_pid; \
	pid_t remote_pid; \
	unsigned long diff_addr; \
	char diff_page[PAGE_SIZE]; \
					\
	int origin_ws; \
	int iter; \
	int total_iter; \
	int merge_id; \
	unsigned long wr_cnt;
DEFINE_PCN_KMSG(page_diff_apply_request_t, PAGE_DIFF_APPLY_REQUEST_FIELDS);

#define REMOTE_BARRIER_DONE_REQUEST_FIELDS \
	pid_t origin_pid; \
	unsigned long remote_region_type;
DEFINE_PCN_KMSG(remote_baiier_done_request_t, REMOTE_BARRIER_DONE_REQUEST_FIELDS);

/**
 * Futex
 */
#define REMOTE_FUTEX_REQ_FIELDS \
	pid_t origin_pid; \
	int remote_ws; \
	int op; \
	u32 val; \
	struct timespec ts; \
	void *uaddr; \
	void *uaddr2; \
	u32 val2; \
	u32 val3;
DEFINE_PCN_KMSG(remote_futex_request, REMOTE_FUTEX_REQ_FIELDS);

#define REMOTE_FUTEX_RES_FIELDS \
	int remote_ws; \
	long ret;
DEFINE_PCN_KMSG(remote_futex_response, REMOTE_FUTEX_RES_FIELDS);

/**
 * Node information
 */
#define NODE_INFO_FIELDS \
	int nid; \
	int bundle_id; \
	int arch;
DEFINE_PCN_KMSG(node_info_t, NODE_INFO_FIELDS);


/**
 * Schedule server. Not yet completely ported though
 */
#define SCHED_PERIODIC_FIELDS \
	int power_1; \
	int power_2; \
	int power_3;
DEFINE_PCN_KMSG(sched_periodic_req, SCHED_PERIODIC_FIELDS);


/**
 * Message routing using work queues
 */
extern struct workqueue_struct *popcorn_wq;
extern struct workqueue_struct *popcorn_wq2;
extern struct workqueue_struct *popcorn_wq3;
extern struct workqueue_struct *popcorn_ordered_wq;

struct pcn_kmsg_work {
	struct work_struct work;
	void *msg;
};

static inline int __handle_popcorn_work(struct pcn_kmsg_message *msg, void (*handler)(struct work_struct *), struct workqueue_struct *wq)
{
//	static u8 mw_cpu = 0;
	struct pcn_kmsg_work *w = kmalloc(sizeof(*w), GFP_ATOMIC);
	BUG_ON(!w);
#if 0
	if (msg->header.type == PCN_KMSG_TYPE_REMOTE_PREFETCH_RESPONSE) {
		remote_prefetch_response_t *res = (remote_prefetch_response_t *)msg;
		printk("\t\t  wq: touched 0x%lx #%d\n",
					res->god_omp_hash, res->pf_req_id);
#endif
#if 0
#define MAX_NAME 255
		char str[MAX_NAME]; int i, ofs = 0;
		//memset(str, 0, MAX_NAME);
		ofs += snprintf(str, MAX_NAME, "%s", "\t\taddrs: ");
		for (i = 0; i < res->pf_list_size; i++) { // TODO data struct
			int ret = res->pf_results[i].result;
			unsigned long addr = res->pf_results[i].addr;
			ofs += snprintf(str + ofs, MAX_NAME, "%s", "0x");
			ofs += snprintf(str + ofs, MAX_NAME, "%lx", addr);
			ofs += snprintf(str + ofs, MAX_NAME, "%s", " ");
			ofs += snprintf(str + ofs, MAX_NAME, "%s", !ret ? "O":"X");
			ofs += snprintf(str + ofs, MAX_NAME, "%s", " ");
		}
		memset(str + ofs, 0, MAX_NAME);
		printk("%s\n", str);
	}
#endif

	w->msg = msg;
	INIT_WORK(&w->work, handler);

	/* TODO PERF: currently enque cpu is decided by irq.
	 * 1. run app and check irq if not balance do 2
	 * 2. workload balancing by ourself */
	//smp_wmb();
	if (msg->header.type != PCN_KMSG_TYPE_REMOTE_PREFETCH_RESPONSE) {
//		&& msg->header.type != PCN_KMSG_TYPE_PAGE_MERGE_REQUEST) {
		BUG_ON(!queue_work(wq, &w->work));
//	} else if (msg->header.type == PCN_KMSG_TYPE_PAGE_MERGE_REQUEST) {
		//BUG_ON(!queue_work(popcorn_wq3, &w->work)); // this makes deadline faster
		//BUG_ON(!queue_work_on(mw_cpu, popcorn_wq3, &w->work));
//		BUG_ON(!queue_work_on(mw_cpu, wq, &w->work));
#if 0
		mw_cpu++;
#if CONFIG_X86_64
		//if (mw_cpu >= (16/4))
		//if (mw_cpu >= (16/2)) // numa
		if (mw_cpu >= 2)
#else
		//if (mw_cpu >= (96/4))
		if (mw_cpu >= 4)
#endif
			mw_cpu = 0;
#endif
	//} else if (msg->header.type == PCN_KMSG_TYPE_REMOTE_PREFETCH_RESPONSE) {
	} else { // == PCN_KMSG_TYPE_REMOTE_PREFETCH_RESPONSE
		//printk("\t\t1: \n");
		//show_workqueue_state();
		BUG_ON(!queue_work(popcorn_wq2, &w->work)); // this makes deadline faster
		//printk("\t\t2: \n");
		//show_workqueue_state();

		//show_pwq(pwq = per_cpu_ptr(wq->cpu_pwqs, cpu);
	}
	//else {
	//}
//	smp_wmb(); // so far so good

#if 0
	if (msg->header.type == PCN_KMSG_TYPE_REMOTE_PREFETCH_RESPONSE) {
		remote_prefetch_response_t *res = (remote_prefetch_response_t *)msg;
		printk("\t\t<-wq: done reqs hash 0x%lx #%d\n",
					res->god_omp_hash, res->pf_req_id);
	}
#endif
	return 0;
}

int request_remote_work(pid_t pid, struct pcn_kmsg_message *req);

#define DEFINE_KMSG_WQ_HANDLER(x) \
static inline int handle_##x(struct pcn_kmsg_message *msg) {\
	return __handle_popcorn_work(msg, process_##x, popcorn_wq);\
}
#define DEFINE_KMSG_ORDERED_WQ_HANDLER(x) \
static inline int handle_##x(struct pcn_kmsg_message *msg) {\
	return __handle_popcorn_work(msg, process_##x, popcorn_ordered_wq);\
}
#define DEFINE_KMSG_RW_HANDLER(x,type,member) \
static inline int handle_##x(struct pcn_kmsg_message *msg) {\
	type *req = (type *)msg; \
	return request_remote_work(req->member, msg); \
}

#define REGISTER_KMSG_WQ_HANDLER(x, y) \
	pcn_kmsg_register_callback(x, handle_##y)

#define REGISTER_KMSG_HANDLER(x, y) \
	pcn_kmsg_register_callback(x, handle_##y)

#define START_KMSG_WORK(type, name, work) \
	struct pcn_kmsg_work *__pcn_kmsg_work__ = (struct pcn_kmsg_work *)(work); \
	type *name = __pcn_kmsg_work__->msg

#define END_KMSG_WORK(name) \
	pcn_kmsg_done(name); \
	kfree(__pcn_kmsg_work__);


#include <linux/sched.h>

static inline struct task_struct *__get_task_struct(pid_t pid)
{
	struct task_struct *tsk = NULL;
	rcu_read_lock();
	tsk = find_task_by_vpid(pid);
	if (likely(tsk)) {
		get_task_struct(tsk);
	}
	rcu_read_unlock();
	return tsk;
}
#endif /* __TYPES_H__ */
