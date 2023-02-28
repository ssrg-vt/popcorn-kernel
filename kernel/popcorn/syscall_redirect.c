#include <linux/module.h>
#include <linux/version.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/kprobes.h>
#include <linux/syscalls.h>
#include <linux/fs.h>
#include <linux/file.h>

#include "syscall_redirect.h"
#include "wait_station.h"
#include "types.h"

const int redirect_table[PCN_NUM_SYSCALLS] = {
  __NR_socket,			// 0
  __NR_setsockopt,
  __NR_bind,
  __NR_listen,
  __NR_accept4,
  __NR_shutdown,		// 5
  __NR_recvfrom,
  __NR_epoll_create1,
  -1/*__NR_epoll_waiti*/,
  __NR_epoll_pwait,
  __NR_epoll_ctl,		// 10
  __NR_read,
  __NR_write,
  -1/* __NR_open*/,
  __NR_close,
  __NR_ioctl,			// 15
  __NR_writev,
  __NR_newfstatat,
  __NR_sendfile,
  -1 /*__NR_select*/,
  __NR_fcntl,			// 20
  -1 /* __NR_stat*/,
  __NR_getpid,
  __NR_getuid,
  __NR_dup,
  __NR_openat,			// 25
  __NR_gettimeofday,
  __NR_statx,
  __NR_pselect6,
  __NR_clock_gettime,
  __NR_nanosleep,
  __NR_futex
};

const char *redirect_table_str[PCN_NUM_SYSCALLS] = {
  "socket",			// 0
  "setsockopt",
  "bind",
  "listen",
  "accept4",
  "shutdown",			// 5
  "recvfrom",
  "epoll_create1",
  "epoll_waiti",
  "epoll_pwait",
  "epoll_ctl",			// 10
  "read",
  "write",
  "open",
  "close",
  "ioctl",			// 15
  "writev",
  "fstat",
  "sendfile",
  "select",
  "fcntl",			// 20
  "stat",
  "getpid",
  "getuid",
  "dup",
  "openat",			// 25
  "gettimeofday",
  "statx",
  "pselect6",
  "clock_gettime",
  "nanosleep",
  "unsupported",
};

const char *popcorn_decode_syscall(int nr)
{
	int i;

	for (i = 0; i < PCN_NUM_SYSCALLS; i++) {
		if (nr == redirect_table[i])
			return redirect_table_str[i];
	}

	return "unsupported";
}

/*
 * Handling the signal sent from origin node to remote node
 * We manually force the signal in the destination PID
 */
int handle_signal_remotes(struct pcn_kmsg_message *msg)
{
       signal_trans_t * recv = (signal_trans_t*)msg;
       struct task_struct * tgt_tsk = find_task_by_vpid(recv->remote_pid);
       printk("%s: received the signal %d for task %d (%d) \n\n",
	      __FUNCTION__, recv->sig, recv->remote_pid,
	      current->pid);
       force_sig(recv->sig, tgt_tsk);
       tgt_tsk->remote->stop_remote_worker = false;
       return 0;
}
EXPORT_SYMBOL(handle_signal_remotes);

/*
 * A signal arrived at the origin node for a process that is currently
 * migrated.We are sending the request to remote node that the process is
 * currently stationed.
 */
int remote_signalling(int sig, struct task_struct * tsk, int group)
{
       int re;
       signal_trans_t *sigreq = pcn_kmsg_get(sizeof(*sigreq));
       sigreq->origin_pid = tsk->pid;
       sigreq->remote_pid = tsk->remote_pid;
       sigreq->remote_nid = tsk->remote_nid;
       sigreq->sig        = sig;
       sigreq->group      = group ? 1:0;
       re = pcn_kmsg_post(PCN_KMSG_TYPE_SIGNAL_FWD,
		       tsk->remote_nid, sigreq, sizeof(*sigreq));
       return 0;
}
EXPORT_SYMBOL(remote_signalling);

int popcorn_syscall_redirectable(unsigned long nr)
{
	int i;

	for (i = 0; i < PCN_NUM_SYSCALLS; i++) {
		if (redirect_table[i] == nr)
			return 1;
	}

	return 0;
}
EXPORT_SYMBOL(popcorn_syscall_redirectable);

long syscall_redirect(unsigned long nr, struct pt_regs *regs)
{
	int ret = 0 , i =0;
	syscall_fwd_t *req = pcn_kmsg_get(sizeof(syscall_fwd_t));
	syscall_rep_t *rep = NULL;
	struct wait_station *ws = get_wait_station(current);

	req->origin_pid = current->origin_pid;
	req->remote_ws = ws->id;
	req->call_type    = -1;

	syscall_get_arg(current,regs,(unsigned long *)&req->args);

	printk("%s: Parameters are %lx, %lx, %lx, %lx, %lx, %lx\n",
	       __FUNCTION__,
	       req->args[0], req->args[1], req->args[2], req->args[3],
	       req->args[4], req->args[5]);

	for (i = 0; i < PCN_NUM_SYSCALLS; i++)
	{
		if(redirect_table[i] == nr)
		{
			req->call_type = i;
			break;
		}
	}

	printk(KERN_INFO "%s: redirect called for #syscall %d (%s) at index %d, %d\n",
	       __FUNCTION__, 
	       nr, redirect_table_str[i], i, PCN_NUM_SYSCALLS);

	if(req->call_type  == -1) {
		BUG_ON( req->call_type  == -1);

		printk(KERN_INFO
		       "%s: redirect called for #syscall %d not yet implemented",
		       __FUNCTION__, nr);

		return -1;
	} else {
		ret = pcn_kmsg_post(PCN_KMSG_TYPE_SYSCALL_FWD, 0,
				req,sizeof(*req));
		rep = wait_at_station(ws);
		ret = rep->ret;
		pcn_kmsg_done(rep);

		printk(KERN_INFO
		       "%s: redirect called for #syscall %d with return value %d, sigpending = %d",
		       __FUNCTION__, nr, ret, rep->sigpending);

		if (rep->sigpending) {
			do_exit(0);
		}
	}
	return ret;
}

static int handle_syscall_reply(struct pcn_kmsg_message *msg)
{
	 syscall_rep_t *rep = (syscall_rep_t *)msg;
	 struct wait_station *ws = wait_station(rep->remote_ws);

	 ws->private = rep;
	 complete(&ws->pendings);
	 return 0;
}

DEFINE_KMSG_RW_HANDLER(syscall_fwd, syscall_fwd_t, origin_pid);

int __init syscall_server_init(void)
{
	REGISTER_KMSG_HANDLER(PCN_KMSG_TYPE_SYSCALL_FWD,
			      syscall_fwd);
	REGISTER_KMSG_HANDLER(PCN_KMSG_TYPE_SYSCALL_REP,
			      syscall_reply);
	REGISTER_KMSG_HANDLER(PCN_KMSG_TYPE_SIGNAL_FWD,
			      signal_remotes);
	return 0;
}
