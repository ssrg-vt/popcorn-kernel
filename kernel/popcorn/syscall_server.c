#include <popcorn/pcn_kmsg.h>
#include <popcorn/types.h>
#include "syscall_server.h"
#include "types.h"
#include "wait_station.h"
#include <linux/socket.h>
#include <linux/unistd.h>
#include <linux/eventpoll.h>
#include <linux/file.h>
#include <linux/types.h>

/* Syscall Definitions are put here*/

/* Define redirection functions*/

/* Process related */
DEFINE_SYSCALL_REDIRECT(getpid, PCN_SYSCALL_GETPID, int, dummy);

/* Socket related */
DEFINE_SYSCALL_REDIRECT(socket, PCN_SYSCALL_SOCKET_CREATE, int, family, int,
			type, int, protocol);
DEFINE_SYSCALL_REDIRECT(setsockopt, PCN_SYSCALL_SETSOCKOPT, int, fd,
			int, level, int, optname, char __user*, optval, int,
			optlen);
DEFINE_SYSCALL_REDIRECT(bind, PCN_SYSCALL_BIND,int, fd, struct sockaddr __user*,
			umyaddr, int, addrlen);
DEFINE_SYSCALL_REDIRECT(listen, PCN_SYSCALL_LISTEN, int, fd, int,
			backlog);
DEFINE_SYSCALL_REDIRECT(accept4, PCN_SYSCALL_ACCEPT4, int, fd, struct
			sockaddr __user*, upper_sockaddr, int __user*,
			upper_addrlen, int, flag);
DEFINE_SYSCALL_REDIRECT(shutdown, PCN_SYSCALL_SHUTDOWN, int, fd, int, how);
DEFINE_SYSCALL_REDIRECT(recvfrom, PCN_SYSCALL_RECVFROM, int, fd, void __user *,
			ubuf, size_t, size, unsigned int, flags,
			struct sockaddr __user *, addr, int __user *, addr_len);

/* Epoll related */
DEFINE_SYSCALL_REDIRECT(epoll_create1, PCN_SYSCALL_EPOLL_CREATE1, int, flags);
DEFINE_SYSCALL_REDIRECT(epoll_wait, PCN_SYSCALL_EPOLL_WAIT, int, epfd,
			struct epoll_event __user *,
			events, int, maxevents, int, timeout);
DEFINE_SYSCALL_REDIRECT(epoll_pwait, PCN_SYSCALL_EPOLL_PWAIT,int, epfd,
			struct epoll_event __user *, events, int, maxevents,
			int, timeout, const sigset_t __user *, sigmask,
			size_t, sigsetsize);
DEFINE_SYSCALL_REDIRECT(epoll_ctl, PCN_SYSCALL_EPOLL_CTL, int, epfd,
			int, op, int, fd, struct epoll_event __user *,
			event);
DEFINE_SYSCALL_REDIRECT(select, PCN_SYSCALL_SELECT, int, n, fd_set __user *,
			inp, fd_set __user *, outp, fd_set __user *, exp,
			struct timeval __user *, tvp);


/* General fs/driver read/write/open/close calls */
DEFINE_SYSCALL_REDIRECT(read, PCN_SYSCALL_READ, unsigned int, fd, char __user*,
			buf, size_t, count);
DEFINE_SYSCALL_REDIRECT(write, PCN_SYSCALL_WRITE, unsigned int, fd, const char
			__user *, buf, size_t, count);
DEFINE_SYSCALL_REDIRECT(open, PCN_SYSCALL_OPEN, const char __user *, filename,
			int, flags, umode_t, mode);
DEFINE_SYSCALL_REDIRECT(close, PCN_SYSCALL_CLOSE, unsigned int, fd);
DEFINE_SYSCALL_REDIRECT(ioctl, PCN_SYSCALL_IOCTL, unsigned int, fd,
			unsigned int, cmd, unsigned long, arg);
DEFINE_SYSCALL_REDIRECT(writev, PCN_SYSCALL_WRITEV, unsigned long,
			fd, const struct iovec __user *, vec,
			unsigned long, vlen);
DEFINE_SYSCALL_REDIRECT(fstat, PCN_SYSCALL_FSTAT, unsigned int, fd,
			struct stat __user *, statbuf);
DEFINE_SYSCALL_REDIRECT(sendfile64, PCN_SYSCALL_SENDFILE64,int, out_fd, int,
			in_fd, loff_t __user *, offset, size_t, count);
DEFINE_SYSCALL_REDIRECT(fcntl, PCN_SYSCALL_FCNTL, unsigned int, fd,
			unsigned int, cmd, unsigned long, arg);
DEFINE_SYSCALL_REDIRECT(fstatat, PCN_SYSCALL_FSTATAT, int, dfd, const char
			__user *, filename, struct stat __user*, statbud, int,
			flag);

/**
 * Syscalls needed in the kernel
 * */
extern int sys_socket(int family, int type, int protocol);
extern int sys_bind(int fd, struct sockaddr __user *umyaddr, int addrlen);
extern int sys_listen(int fd, int backlog);
extern int sys_accept4(int fd, struct sockaddr __user *upeer_sockaddr,
		     int __user *upeer_addrlen, int flag);
extern int sys_setsockopt(int fd, int level, int optname, char __user *optval,
			  int optlen);
extern long sys_recvfrom(int, void __user *, size_t, unsigned,
				struct sockaddr __user *, int __user *);
extern long sys_shutdown(int, int);
extern long sys_epoll_create1(int flags);
extern long sys_epoll_ctl(int epfd, int op, int fd,
				struct epoll_event __user *event);
extern long sys_epoll_wait(int epfd, struct epoll_event __user *events,
				int maxevents, int timeout);
extern long sys_epoll_pwait(int epfd, struct epoll_event __user *events,
				int maxevents, int timeout,
				const sigset_t __user *sigmask,
				size_t sigsetsize);
extern long sys_read(unsigned int fd, char __user *buf, size_t count);
extern long sys_write(unsigned int fd, const char __user *buf, size_t count);
extern long sys_open(const char __user *filename, int flags, umode_t mode);
extern long sys_close(unsigned int fd);
extern long sys_ioctl(unsigned int fd, unsigned int cmd, unsigned long arg);
extern long sys_writev(unsigned long fd,
			   const struct iovec __user *vec,
			   unsigned long vlen);
extern long sys_newfstat(unsigned int fd, struct stat __user *statbuf);
extern long sys_sendfile64(int out_fd, int in_fd,
			       loff_t __user *offset, size_t count);
extern long sys_select(int n, fd_set __user *inp, fd_set __user *outp,
			fd_set __user *exp, struct timeval __user *tvp);
extern long sys_fcntl(unsigned int fd, unsigned int cmd, unsigned long arg);
extern long sys_newfstatat(int dfd, const char __user *filename,
			       struct stat __user *statbuf, int flag);
extern long sys_getpid(void);

/*
 * Handling the signal sent from origin node to remote node
 * We manually force the signal in the destination PID
 */
int handle_signal_remotes(struct pcn_kmsg_message  *msg)
{
       signal_trans_t * recv = (signal_trans_t*)msg;
       struct task_struct * tgt_tsk = find_task_by_vpid(recv->remote_pid);
       printk(KERN_INFO"received the signal %d for task %d \n\n",
                       recv->sig,recv->remote_pid);
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
int remote_signalling(int sig ,struct task_struct * tsk , int group )
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


int process_remote_syscall(struct pcn_kmsg_message *msg)
{
	int retval = 0;
	syscall_fwd_t *req = (syscall_fwd_t *)msg;
	syscall_rep_t *rep = pcn_kmsg_get(sizeof(*rep));

	/*Call the original system call and pass in delivered params. */
	switch(req->call_type) {
	case PCN_SYSCALL_SOCKET_CREATE:
		retval = sys_socket((int)req->param0, (int)req->param1,
				    (int)req->param2);
		break;
	case PCN_SYSCALL_SETSOCKOPT:
		retval = sys_setsockopt((int)req->param0, (int)req->param1,
					(int)req->param2,
					(char __user*)req->param3,
					(int)req->param4);
		break;
	case PCN_SYSCALL_BIND:
        retval = sys_bind((int)req->param0, (struct sockaddr __user*)
				  req->param1, (int)req->param2);
		break;
	case PCN_SYSCALL_LISTEN:
		retval = sys_listen((int)req->param0, (int)req->param1);
		break;
	case PCN_SYSCALL_ACCEPT4:
		retval = sys_accept4((int)req->param0,
				     (struct sockaddr __user*)req->param1,
				     (int __user*)req->param2,
				     (int)req->param3);
		break;
	case PCN_SYSCALL_SHUTDOWN:
		retval = sys_shutdown((int)req->param0, (int)req->param1);
		break;
	/* Event poll related syscalls */
	case PCN_SYSCALL_EPOLL_CREATE1:
		retval = sys_epoll_create1((int)req->param0);
		break;
	case PCN_SYSCALL_EPOLL_WAIT:
		retval = sys_epoll_wait((int)req->param0,
				(struct epoll_event __user *)req->param1,
				(int)req->param2, (int)req->param3);
		break;
	case PCN_SYSCALL_EPOLL_CTL:
		retval = sys_epoll_ctl((int)req->param0, (int)req->param1,
				       (int)req->param2, (struct epoll_event
				       __user *)req->param3);
		break;

	case PCN_SYSCALL_READ:
		retval = sys_read((unsigned int)req->param0,
				  (char __user *)req->param1,
				  (size_t) req->param2);
		break;
	case PCN_SYSCALL_WRITE:
		retval = sys_write((unsigned int)req->param0,
				  (const char __user *)req->param1,
				  (size_t) req->param2);
		break;
	case PCN_SYSCALL_OPEN:
		retval = sys_open((const char __user *)req->param0,
				  (int)req->param1,
				  (umode_t)req->param2);
		break;
	case PCN_SYSCALL_CLOSE:
		retval = sys_close((unsigned int)req->param0);
		break;
	case PCN_SYSCALL_IOCTL:
		retval = sys_ioctl((unsigned int)req->param0,
				   (unsigned int)req->param1,
				   (unsigned long)req->param2);
		break;
	case PCN_SYSCALL_WRITEV:
		retval = sys_writev((unsigned long)req->param0,
				   (const struct iovec __user *)req->param1,
				   (unsigned long)req->param2);
		break;
	case PCN_SYSCALL_RECVFROM:
		retval = sys_recvfrom((int)req->param0,
				   (void __user *)req->param1,
				   (size_t)req->param2,
				   (unsigned)req->param3,
				   (struct sockaddr __user *)req->param4,
				   (int __user *)req->param5);
		break;
	case PCN_SYSCALL_FSTAT:
		retval = sys_newfstat((unsigned int)req->param0,
				   (struct stat __user *)req->param1);
		break;
	case PCN_SYSCALL_SENDFILE64:
		retval = sys_sendfile64((int)req->param0, (int)req->param1,
			       (loff_t __user *)req->param2, (size_t)req->param3);
		break;
	case PCN_SYSCALL_EPOLL_PWAIT:
		retval = sys_epoll_pwait((int)req->param0, (struct epoll_event
							    __user *)req->param1,
				(int)req->param2, (int)req->param3,
				(const sigset_t __user *)req->param4,
				(size_t)req->param5);
		break;
	case PCN_SYSCALL_SELECT:
		retval = sys_select((int) req->param0, (fd_set __user*)
				req->param1, (fd_set __user *)req->param2,
				(fd_set __user *)req->param3,
				(struct timeval __user *)req->param4);
		break;
	case PCN_SYSCALL_FCNTL:
		retval = sys_fcntl((unsigned int) req->param0, (unsigned int)
				req->param1, (unsigned long)req->param2);
		break;
	case PCN_SYSCALL_FSTATAT:
		retval = sys_newfstatat((int) req->param0, (const char __user*)
				req->param1,
			       (struct stat __user *)req->param2, (int)
			       req->param3);
		break;
	case PCN_SYSCALL_GETPID:
		retval = sys_getpid();
		break;
	default:
		retval = -EINVAL;
	}
	rep->origin_pid = current->origin_pid;
	rep->remote_ws = req->remote_ws;
	rep->ret = retval;
	pcn_kmsg_post(PCN_KMSG_TYPE_SYSCALL_REP, current->remote_nid, rep,
		      sizeof(*rep));
	pcn_kmsg_done(req);

	return retval;
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
