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

#define FAULTS_HASH 		31
#define MVX_MSG_SIZE		1024
#define MVX_EXE_PATH_SIZE	1024
#define MVX_CMDLINE_SIZE	1024

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

typedef struct popcorn_fd {
    unsigned int idx;
    char file_path[128];
} fd_t;

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
    fd_t fds[64];\
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

#define DEV_ZERO_STRING	"/dev/zero"
/* -1 because we want to partial match, no inclusion of '\0' */
#define DEV_ZERO_STRING_LEN (sizeof(DEV_ZERO_STRING) - 1)
#define vma_info_anon(x) ((x)->vm_file_path[0] == '\0' ? true : false)
#define vma_info_dev_zero(x) (!strncmp((x)->vm_file_path, DEV_ZERO_STRING,\
			      DEV_ZERO_STRING_LEN))

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
 * Syscall server. Allows forwarding and handling of remote system calls.
 */

/* Enumerate syscall types*/
enum pcn_syscall_types
{
	PCN_SYSCALL_SOCKET_CREATE,	// 0
	PCN_SYSCALL_SETSOCKOPT,
	PCN_SYSCALL_BIND,
	PCN_SYSCALL_LISTEN,
	PCN_SYSCALL_ACCEPT4,
	PCN_SYSCALL_SHUTDOWN,		// 5
	PCN_SYSCALL_RECVFROM,
	PCN_SYSCALL_EPOLL_CREATE1,
	PCN_SYSCALL_EPOLL_WAIT,
	PCN_SYSCALL_EPOLL_PWAIT,
	PCN_SYSCALL_EPOLL_CTL,		// 10
	PCN_SYSCALL_READ,
	PCN_SYSCALL_WRITE,
	PCN_SYSCALL_OPEN,
	PCN_SYSCALL_CLOSE,
	PCN_SYSCALL_IOCTL,		// 15
	PCN_SYSCALL_WRITEV,
	PCN_SYSCALL_FSTAT,
	PCN_SYSCALL_SENDFILE64,
	PCN_SYSCALL_SELECT,
	PCN_SYSCALL_FCNTL,		// 20
	PCN_SYSCALL_FSTATAT,
	PCN_SYSCALL_GETPID,
	PCN_NUM_SYSCALLS
};

#define SYSCALL_FWD_FIELDS				\
	pid_t origin_pid;				\
	uint64_t param0;				\
	uint64_t param1;				\
	uint64_t param2;				\
	uint64_t param3;				\
	uint64_t param4;				\
	uint64_t param5;				\
	int remote_ws;					\
	enum pcn_syscall_types call_type;		\
	int ret;
DEFINE_PCN_KMSG(syscall_fwd_t, SYSCALL_FWD_FIELDS);

#define SYSCALL_REP_FIELDS				\
	pid_t origin_pid;				\
	int remote_ws;					\
	int ret;
DEFINE_PCN_KMSG(syscall_rep_t, SYSCALL_REP_FIELDS);

#define SIGNAL_TRANSMIT_FIELDS 				\
       pid_t origin_pid;                                \
       pid_t remote_pid;                                \
       int   remote_nid;                                \
       int sig;                                         \
       bool group;
DEFINE_PCN_KMSG(signal_trans_t , SIGNAL_TRANSMIT_FIELDS);

/* Command line struct */
typedef struct _cmd {
	int argc;
	int max_arg_len;
	int cmdline_size;
	char cmdline[MVX_CMDLINE_SIZE];
	char exe_path[MVX_EXE_PATH_SIZE];
} cmd_t;

/**
 * MVX message.
 * */
#define MVX_INIT_FIELDS				\
	pid_t origin_pid;			\
	cmd_t cmd;
DEFINE_PCN_KMSG(mvx_init_t, MVX_INIT_FIELDS);

#define MVX_MSG_FIELDS				\
	short syscall;				\
	short flag;				\
	unsigned int len;			\
	long retval;				\
	char buf[MVX_MSG_SIZE];
DEFINE_PCN_KMSG(mvx_message_t, MVX_MSG_FIELDS);

#define MVX_MSG_REPLY_FIELDS			\
	long retval;				\
	long syscall;
DEFINE_PCN_KMSG(mvx_reply_t, MVX_MSG_REPLY_FIELDS);

/* MVX core message. */
typedef struct {
	MVX_MSG_FIELDS;		/* kernel/popcorn/types.h */
} __attribute__((packed)) mvx_core_msg_t;

/**
 * Message routing using work queues
 */
extern struct workqueue_struct *popcorn_wq;
extern struct workqueue_struct *popcorn_ordered_wq;

struct pcn_kmsg_work {
	struct work_struct work;
	void *msg;
};

static inline int __handle_popcorn_work(struct pcn_kmsg_message *msg, void (*handler)(struct work_struct *), struct workqueue_struct *wq)
{
	struct pcn_kmsg_work *w = kmalloc(sizeof(*w), GFP_ATOMIC);
	BUG_ON(!w);

	w->msg = msg;
	INIT_WORK(&w->work, handler);
	smp_wmb();
	queue_work(wq, &w->work);

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
