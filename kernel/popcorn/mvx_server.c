#include <linux/kernel.h>
#include <linux/fs.h>		// getname_kernel()
#include <linux/completion.h>	// completion
#include <linux/syscalls.h>

#include <popcorn/mvx.h>
#include <popcorn/debug.h>	// MVXPRINTK()
#include "types.h"
#include "util.h"		// get_file_path()

/* Defines an array for syscall args; used for passing data between modules. */
#define NUM_SYSCALLS	6
int64_t mvx_args[NUM_SYSCALLS];
EXPORT_SYMBOL(mvx_args);

/* MVX master/follower node id. */
int master_nid = 1;	// arm64 as master
int follower_nid = 0;

/* A global mvx message; used for passing data between modules. */
mvx_core_msg_t mvx_follower_msg;

DECLARE_COMPLETION(master_wait);	// The completion used by master to sync
EXPORT_SYMBOL(master_wait);

DECLARE_COMPLETION(follower_wait);	// The completion used by follower to sync
EXPORT_SYMBOL(follower_wait);

/* Flags to enable/disable MVX mode process execution. */
bool master_mvx_on;
bool follower_mvx_on;

/* The Virtual Descriptor Table */
int fd_vtab[VDT_SIZE];
/* Points to the next available fd. */
int vtab_count = 0;	// not include 0, 1, 2.

/* The syscall translation table. */
const int syscall_tbl_a2x[512] = {
#include <popcorn/sctbl_arm_x86.h>
};
const int syscall_tbl_x2a[512] = {
#include <popcorn/sctbl_x86_arm.h>
};
int syscall_tbl[512];

/* The string table for syscall names. */
char* syscall_name[512] = {
#ifdef __x86_64__
#include <popcorn/syscall_x86.h>
#endif
#ifdef __aarch64__
#include <popcorn/syscall_arm.h>
#endif
};

/* A list of whitelist dir. */
const char* dir_whitelist[] = {
#include <popcorn/whitelist.h>
};
size_t whitelist_len = sizeof(dir_whitelist)/sizeof(char*);

/* ========= Common code for MVX system ========= */
/* Initialize the FD VTable. */
static inline void init_mvx_vtab(void)
{
	memset(fd_vtab, 0, sizeof(int)*VDT_SIZE);
	fd_vtab[1] = fd_vtab[2] = MVX_REAL;
	vtab_count = 0;

	mvx_index = 0;	// This is not vtab, but we need to clear it up on initialization
}

/* Initialize the MVX process flags and the syscall translation table. */
static inline void init_mvx_proc_syscall(struct task_struct *tsk, int leader)
{
	if (leader) {
		/* This is master. */
		tsk->is_follower = 0;
#ifdef __x86_64__
		memcpy(syscall_tbl, syscall_tbl_x2a, 512*sizeof(int));
#endif
#ifdef __aarch64__
		memcpy(syscall_tbl, syscall_tbl_a2x, 512*sizeof(int));
#endif
		MVXPRINTK("MVX master variant [PID:%d] init ...\n", tsk->pid);
	} else {
		/* This is follower. */
		tsk->is_follower = 1;
		MVXPRINTK("MVX follower variant [PID:%d] init ...\n", tsk->pid);
	}
	/* MVX process. */
	tsk->is_mvx_process = 1;
}

/* ========= Syscall code for MVX system ========= */
/**
 * Kernel code entry for MVX (syscall using LD_PRELOAD).
 */
SYSCALL_DEFINE1(popcorn_mvx, int, mvx_leader)
{
	pr_info("=== Popcorn MVX ===\n");
	pr_info("Using LD_PRELOAD=./loader.so <bin>");
	pr_info(" 1) Start on master and 2) on follower\n");
	pr_info("%d %d\n", MVX_VFD_MAX, MVX_OFF);

	init_mvx_proc_syscall(current, mvx_leader);
	init_mvx_vtab();

	return 0;
}

/**
 * Syscall for debugging purpose.
 * */
asmlinkage long sys_hscall(unsigned long arg0, unsigned long arg1,
                               unsigned long arg2, unsigned long arg3,
                               unsigned long arg4, unsigned long arg5)
{
	pr_info("=== Popcorn MVX ===\n");
	pr_info("Using LD_PRELOAD=./loader.so <bin>");
	pr_info("Args: 0x%lx(%ld), 0x%lx, 0x%lx, 0x%lx, 0x%lx, 0x%lx\n",
	       arg0, arg0, arg1, arg2, arg3, arg4, arg5);

	/* arg0 == 0x1 indicates a MVX process (master). */
	if (arg0 == 0x1) {
		mvx_server_start_mvx(current, 0);	// TODO: dst node

		init_mvx_proc_syscall(current, 1);
		init_mvx_vtab();
		//MVXPRINTK("Master variant [PID:%d] Init ...\n", current->pid);
	}

#if 0
	/* arg1 indicates a master (0) or a follower (non-0). */
	if (arg1 == 0x0) {
		// This is master.
		current->is_follower = 0;
		MVXPRINTK("Master Init ...\n");

		/* Only master needs the syscall translation table. */
#ifdef __x86_64__
		memcpy(syscall_tbl, syscall_tbl_x2a, 512*sizeof(int));
#endif
#ifdef __aarch64__
		memcpy(syscall_tbl, syscall_tbl_a2x, 512*sizeof(int));
#endif
	}
	else {
		// This is follower.
		current->is_follower = 1;
		MVXPRINTK("Follower Init ...\n");
	}

	init_mvx_vtab();
#endif

	return 0;
}

/* ========= Code executed on Follower ========= */
static DEFINE_SPINLOCK(mvx_lock);
/**
 * This function initiates the MVX follower process.
 * Some code are from 'call_usermodehelper', 'do_execve' will replace 
 * the current thread context with the binary provided as the 1st param.
 * */
static int mvx_process_fork(void *data)
{
	char *path = (char *)data;
	char *argv[] = {path, NULL};
	static char *envp[] = {"HOME=/", "TERM=linux",
		"PATH=/sbin:/bin:/usr/sbin:/usr/bin", NULL};

	struct cred *new;
	int retval = -ENOMEM;

	spin_lock_irq(&current->sighand->siglock);
	flush_signal_handlers(current, 1);
	spin_unlock_irq(&current->sighand->siglock);

	set_user_nice(current, 0);

	new = prepare_kernel_cred(current);
	if (!new)
		goto out;
	spin_lock(&mvx_lock);
	new->cap_bset = cap_intersect(CAP_FULL_SET, new->cap_bset);
	new->cap_inheritable = cap_intersect(CAP_FULL_SET,
					     new->cap_inheritable);
	spin_unlock(&mvx_lock);
	commit_creds(new);

	// MVX process set up.
	init_mvx_proc_syscall(current, 0);
	init_mvx_vtab();

	retval = do_execve(getname_kernel(path),
			   (const char __user *const __user *)argv,
			   (const char __user *const __user *)envp);
	MVXPRINTK("%s: do_execve ret %d\n", __func__, retval);
	if (!retval)
		return 0;
out:
	do_exit(0);
}

/**
 * The thread for MVX work (workqueue)
 * */
static void clone_mvx_thread(struct work_struct *_work)
{
	struct pcn_kmsg_work *work = (struct pcn_kmsg_work *)_work;
	mvx_init_t *init_msg = work->msg;
	pid_t pid;
	struct task_struct *mvx_task;

	// Use a kthread to find the userspace process and mark it as MVX process
	pid = kernel_thread(mvx_process_fork, init_msg->exe_path,
			    CLONE_PARENT | SIGCHLD);
	if (pid < 0) {
		printk("%s: create child process error\n", __func__);
		return;
	}
	MVXPRINTK("%s: thread [%d] created, path %s\n",
		 __func__, pid, init_msg->exe_path);

	// Set it as MVX (follower) process
	mvx_task = pid_task(find_vpid(pid), PIDTYPE_PID);
	mvx_task->is_mvx_process = 1;
	mvx_task->is_follower = 1;
	MVXPRINTK("%s: comm %s. mvx process %d, follower %d\n", __func__,
		 mvx_task->comm, mvx_task->is_mvx_process,
		 mvx_task->is_follower);
}

/**
 * Follower handles MVX requests
 * Both pcn_kmsg_message and mvx_init_t have the same header.
 * */
static int handle_mvx_start(struct pcn_kmsg_message *msg)
{
	mvx_init_t *init_msg = (mvx_init_t *)msg;
	struct pcn_kmsg_work *work = kmalloc(sizeof(*work), GFP_ATOMIC);
	BUG_ON(!work);
	work->msg = init_msg;

	INIT_WORK((struct work_struct *)work, clone_mvx_thread);
	queue_work(popcorn_wq, (struct work_struct *)work);

	return 0;
}

/**
 * Follower handles MVX message.
 * */
static int handle_mvx_message(struct pcn_kmsg_message *msg)
{
	mvx_message_t *req = (mvx_message_t *)msg;
	ssize_t len = 0;
	extern struct completion follower_wait;

	MVXPRINTK("--> %s: syscall #%d, flag %d, len %u, retval %ld (master)\n",
		  __func__, req->syscall, req->flag, req->len, req->retval);
	if (req->len) MVXPRINTK("--> buf: %s\n", req->buf);

	/* Copy mvx_message_t to a local mvx_follower_msg variable. */
	memcpy(&mvx_follower_msg, (char*)req + sizeof(struct pcn_kmsg_hdr),
		sizeof(mvx_core_msg_t));
	len = mvx_follower_msg.len;	// TODO: currently the msg buf is < 1K
	mvx_follower_msg.buf[len] = 0;

	/* Notify mvx_follower_wait_exec() to continue execute. */
	complete(&follower_wait);

	pcn_kmsg_done(msg);

	return 0;
}

/* ========= Code executed on Master ========= */
/**
 * Master handles MVX reply message.
 * */
static int handle_mvx_reply(struct pcn_kmsg_message *msg)
{
#ifdef CONFIG_POPCORN_DEBUG_MVX_SERVER
	mvx_reply_t *reply = (mvx_reply_t *)msg;
#endif
	extern struct completion master_wait;

	MVXPRINTK("--> %s: ret 0x%lx(%ld), follower syscall %ld\n", __func__,
		  reply->retval, reply->retval, reply->syscall);
	complete(&master_wait);

	pcn_kmsg_done(msg);

	return 0;
}

/**
 * Master sends a quick reply msg to follower.
 * */
int mvx_send_reply(long retval, long syscall, int dst_nid)
{
	mvx_reply_t *reply;

	reply = pcn_kmsg_get(sizeof(*reply));
	reply->retval = retval;
	reply->syscall = syscall;

	pcn_kmsg_post(PCN_KMSG_TYPE_MVX_REPLY, dst_nid, reply,
			    sizeof(*reply));
	//pcn_kmsg_done(reply);	// TODO: pcn_kmsg_put(reply); //?
	return 0;
}

/**
 * Master initializes MVX, and syncronizes with remote follower(s)
 * The real syscall handler.
 * */
int mvx_server_start_mvx(struct task_struct *tsk, unsigned int dst_nid)
{
	struct mm_struct *mm = get_task_mm(tsk);
	mvx_init_t *req;
	int ret;

	req = pcn_kmsg_get(sizeof(*req));	// alloc msg data
	if (!req) {
		ret = -ENOMEM;
		goto out;
	}

	// Get the exe file name and path.
	if (get_file_path(mm->exe_file, req->exe_path, sizeof(req->exe_path))) {
		printk("%s: cannot get path to exe binary\n", __func__);
		ret = -ESRCH;
		pcn_kmsg_put(req);
		goto out;
	}

	MVXPRINTK("%s: [%d] exe_path %s. mvx %d, follower %d\n", __func__,
		 tsk->pid, req->exe_path, tsk->is_mvx_process, tsk->is_follower);
	MVXPRINTK("%s: size %lu; dst nid %d\n", __func__, sizeof(*req), dst_nid);

	ret = pcn_kmsg_send(PCN_KMSG_TYPE_MVX, dst_nid, req, sizeof(*req));

	pcn_kmsg_put(req);
out:
	return ret;
}

/**
 * Initialize the MVX server.
 * Register the MVX (messaging) event handlers.
 */
int __init mvx_server_init(void)
{
	/* Follower variant handles the MVX start message from master. */
	REGISTER_KMSG_HANDLER(PCN_KMSG_TYPE_MVX, mvx_start);
	/* Follower variant handles the MVX (syscall sync) message from master. */
	REGISTER_KMSG_HANDLER(PCN_KMSG_TYPE_MVX_MSG, mvx_message);

	/* Master variant handles the MVX (syscall) replay message from follower. */
	REGISTER_KMSG_HANDLER(PCN_KMSG_TYPE_MVX_REPLY, mvx_reply);
	//init_mvx_vtab();

	return 0;
}
