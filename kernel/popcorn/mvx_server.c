#include <linux/kernel.h>
#include <linux/fs.h>		// getname_kernel()
#include <linux/fdtable.h>
#include <linux/completion.h>	// completion
#include <linux/syscalls.h>
#include <linux/module.h>	// call_usermodehelper_*
#include <linux/cred.h>
#include <linux/uidgid.h>

#include <popcorn/mvx.h>
#include <popcorn/debug.h>	// MVXPRINTK()
#include "types.h"
#include "util.h"		// get_file_path()

/* Defines an array for syscall args; used for passing data between modules. */
#define NUM_SYSCALLS	6
int64_t mvx_args[NUM_SYSCALLS];
EXPORT_SYMBOL(mvx_args);

/* MVX master/follower node id. */
//int master_nid = 1;	// arm64 as master
//int follower_nid = 0;

/* A global mvx message; used for passing data between modules. */
mvx_core_msg_t mvx_follower_msg;

DECLARE_COMPLETION(master_wait);	// The completion used by master to sync
EXPORT_SYMBOL(master_wait);

DECLARE_COMPLETION(follower_wait);	// The completion used by follower to sync
EXPORT_SYMBOL(follower_wait);

/* The Virtual Descriptor Table */
int fd_vtab[VDT_SIZE];
/* Points to the next available fd. */
int vtab_count = 0;	// not include 0, 1, 2.

/* Pseudo file locations for follower fd 0, 1 and 2. */
#define MVX_STDIN_LOC		"/dev/pts/0"
#define MVX_STDOUT_LOC		"/dev/pts/0"

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

	mvx_index = 0;	// This is not for vtab, but for syscall seq log.
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
	pr_info("\n=== HeterSec syscall ===\n");
	pr_info("Arg0: 0x%lx(%ld)\n", arg0, arg0);

	/* arg0 == 0x1 indicates a MVX process (master). */
	if (arg0 == 0x1) {
		/* Get the process info and message follower to start. */
		mvx_server_start_mvx(current, FOLLOWER_NID);
		/* Setup the MVX process info. */
		init_mvx_proc_syscall(current, 1);
		/* Init FD vtab. */
		init_mvx_vtab();
	}
	if (arg0 == 0x2) {
		//retrieve_argv(current);
	}

	return 0;
}

/* ========= Code executed on Follower ========= */
/**
 * Setup and install the pseudo FDs (STDIN, STDOUT, STDERR).
 * */
static bool install_pseudo_fd(char *stdin, char *stdout)
{
	struct file *fp_stdin, *fp_stdout;
	struct files_struct *files = current->files;
	struct fdtable *fdtab = NULL;

	if (files == NULL) {
		MVXPRINTK("Cannot find open file table struct.\n");
		goto out;
	}
	/* Alloc and open fd 0, 1 and 2. */
	fdtab = files_fdtable(files);
	if ((__alloc_fd(files, 0, rlimit(RLIMIT_NOFILE), 0) < 0)
		|| (__alloc_fd(files, 1, rlimit(RLIMIT_NOFILE), 0) < 0)
		|| (__alloc_fd(files, 2, rlimit(RLIMIT_NOFILE), 0) < 0)) {
		MVXPRINTK("%s: __alloc_fd error.\n", __func__);
		goto out;
	}

	fp_stdin = filp_open(stdin, O_RDONLY|O_CREAT, 0644);
	if (IS_ERR(fp_stdin)) {
		MVXPRINTK("Cannot open STDIN file %ld.\n", PTR_ERR(fp_stdin));
		return false;
	}
	fdtab->fd[0] = fp_stdin;

	fp_stdout = filp_open(stdout, O_WRONLY|O_CREAT|O_TRUNC, 0644);
	if (IS_ERR(fp_stdin)) {
		MVXPRINTK("Cannot open STDOUT (STDERR) files %ld.\n", PTR_ERR(fp_stdout));
		return false;
	}
	fdtab->fd[1] = fdtab->fd[2] = fp_stdout;

	return true;
out:
	return false;
}

/**
 * Update uid and gid in `cred *new` with the id provided.
 * */
static void alter_uid_gid(struct cred *new, uid_t uid, gid_t gid)
{
	new->uid = new->euid = new->suid = new->fsuid = KUIDT_INIT(uid);
	new->gid = new->egid = new->sgid = new->fsgid = KGIDT_INIT(gid);
}

/**
 * Callback routine to init the MVX follower variant process.
 * - Mark the process as MVX follower process
 * - Init the fd vtab
 * - Install the pseudo fds.
 * */
static int init_mvx_follower_process(struct subprocess_info *info, struct cred *new)
{
	init_mvx_proc_syscall(current, 0);
	init_mvx_vtab();
	install_pseudo_fd(MVX_STDIN_LOC, MVX_STDOUT_LOC);
	alter_uid_gid(new, MVX_FOLLOWER_UID, MVX_FOLLOWER_GID);

	MVXPRINTK("[%d] %s: Finished the Follower MVX process initialization.\n",
		current->pid, __func__);

	return 0;
}

/**
 * This function initiates the MVX follower process.
 * It launches the follower process by calling 'call_usermodehelper'.
 * The init_func initiates the vtab, fake fd, etc.
 * */
static int mvx_process_fork(void *data)
{
	struct subprocess_info *sub_info;
	int i, off, arg_size, ret = 0;

	cmd_t *cmd = (cmd_t *)data;
	char *path = cmd->exe_path;
	char *cmdline = cmd->cmdline;
	int argc = cmd->argc;
	char *argv[8] = {0};	/* Only support up to 8 params for now. */
	static char *envp[] = {"HOME=/", "TERM=linux",
		"PATH=/sbin:/bin:/usr/sbin:/usr/bin", NULL};

	/* Prepare the argv[] of the follower variant process. */
	BUG_ON(argc > 8);	/* Only support up to 8 params for now. */
	off = 0;
	for (i = 0; i < argc; i++) {
		arg_size = strlen(cmdline + off);
		argv[i] = cmdline + off;
		off += (arg_size + 1);
		MVXPRINTK("argv[%u] %s (%u)\n", i, argv[i], arg_size);
	}
	argv[0] = path;	/* argv[0] points to the full path of the executable. */

	/* Prepare the userspace follower variant. */
	sub_info = call_usermodehelper_setup(argv[0], argv, envp, GFP_ATOMIC,
			init_mvx_follower_process, NULL, NULL);
	if (sub_info == NULL) return -ENOMEM;

	ret = call_usermodehelper_exec(sub_info, UMH_KILLABLE);
	MVXPRINTK("%s: Finished MVX process setup. ret %d\n", __func__, ret);
	do_exit(0);

	return ret;
}

/**
 * The thread for MVX work (workqueue)
 * */
static void clone_mvx_thread(struct work_struct *_work)
{
	struct pcn_kmsg_work *work = (struct pcn_kmsg_work *)_work;
	mvx_init_t *init_msg = work->msg;
	pid_t pid;

	/* Use a kthread to launch a userspace process and mark it as MVX process */
	pid = kernel_thread(mvx_process_fork, &(init_msg->cmd),
			    CLONE_PARENT | SIGCHLD);
	if (pid < 0) {
		printk("%s: create child process error\n", __func__);
		return;
	}
	MVXPRINTK("%s: MVX helper thread [%d] finished launching MVX process, bin path: %s.\n",
			__func__, pid, init_msg->cmd.exe_path);
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
	mvx_reply_t *reply = (mvx_reply_t *)msg;
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
 * Prepare the cmd: retrieve master variant's cmdline from `mm_struct`.
 * */
static int mvx_prepare_init_req(struct mm_struct *mm, cmd_t *cmd)
{
	int ret, i, argc, arg_size;
	char *buf = cmd->cmdline;

	/* Get cmdline and size of the current process. */
	ret = get_cmdline(current, cmd->cmdline, MVX_CMDLINE_SIZE);
	if (ret >= MVX_CMDLINE_SIZE) {
		MVXPRINTK("%s: MVX_CMDLINE_SIZE %d too small for cmdline\n",
			__func__, MVX_CMDLINE_SIZE);
		return -ENOMEM;
	}
	cmd->cmdline_size = ret;

	/* Get the exe file path. */
	if (get_file_path(mm->exe_file, cmd->exe_path, sizeof(cmd->exe_path))) {
		MVXPRINTK("%s: get_file_path error\n", __func__);
		return -ENOMEM;
	}

	/* Get argc by parsing the cmdline. */
	argc = arg_size = 0;
	for (i = 0; i < ret; i += (arg_size+1)) {
		arg_size = strlen(buf + i);
		pr_info("[%2u] argv[%d]: %s. arg_size %u\n", i, argc++, buf+i, arg_size);
	}
	cmd->argc = argc;

	MVXPRINTK("%s: Retrieved argc %u, cmdline size %u, cmdline %s, exe path %s\n",
			__func__, cmd->argc, cmd->cmdline_size, cmd->cmdline, cmd->exe_path);

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

	/* Prepare 'cmdline' and 'exe_path + executable name' from `mm_struct`. */
	if (mvx_prepare_init_req(mm, &(req->cmd))) {
		printk("%s: Prepare cmd from mm_struct error.\n", __func__);
		ret = -ESRCH;
		pcn_kmsg_put(req);
		goto out;
	}

	MVXPRINTK("%s: [%d] exe_path %s. arg_start 0x%lx, arg_end 0x%lx\n", __func__,
			tsk->pid, req->cmd.exe_path, mm->arg_start, mm->arg_end);
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

	return 0;
}
