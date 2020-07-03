#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/syscalls.h>	// kernel syscalls sys_xxx (e.g., sys_dup)
#include <linux/completion.h>	// completion
#include <linux/eventpoll.h>	// struct epoll_event
#include <asm/uaccess.h>	// copy_from_user()
#include <asm/unistd.h>		// __NR_read

#include <popcorn/mvx.h>
#include <popcorn/debug.h>
#include "types.h"

extern mvx_core_msg_t mvx_follower_msg;
extern int fd_vtab[VDT_SIZE];
extern int vtab_index;
extern int syscall_tbl[512];

extern char* syscall_name[512];
extern char* syscall_name_arm[512];

//#ifdef CONFIG_POPCORN_DEBUG_MVX_SERVER
long int mvx_index = 0;
//#endif

//==================================================================
//======================= Follower routines ========================
/**
 * ssize_t read(int fd, void *buf, size_t count);
 * */
static int follower_read(int64_t args[], int32_t *retval)
{
	int ret = 0;
	int64_t *ret64 = (int64_t *)retval;
	//char __user *buf = (char __user *)args[1];
	char *buf = (char *)args[1];
	ssize_t len = strlen(mvx_follower_msg.buf);

	MVXPRINTK("%s: fd %lld, sizeof buf %lu, flag %d, buf %s\n",
		  __func__, args[0], len, mvx_follower_msg.flag,
		  mvx_follower_msg.buf);
	mvx_print_fd_vtab();

	/* if not a real fd read. */
	if (mvx_follower_msg.flag == MVX_SIM) {
		*ret64 = mvx_follower_msg.retval;
		if (*ret64 > 0) {
			// ret of copy_to_user is the number of bytes that are not copied.
			ret = copy_to_user(buf, mvx_follower_msg.buf, len);
			BUG_ON(ret);
		} else if (*ret64 == 0) {
			char zero = 0;
			//buf[0] = 0;
			ret = copy_to_user(buf, &zero, sizeof(char));
			BUG_ON(ret);
		} else {
			if (*retval == -ERESTARTSYS)	// ctrl-c
				do_exit(130);
		}
		return 0;  // simulation: follower not need to process post-syscall
	}
	return 1;
}

/**
 * int epoll_pwait(int epfd, struct epoll_event *events,
 *		int maxevents, int timeout, const sigset_t *sigmask);
 * Cares about events (args[1]) and retval.
 * */
static void follower_epoll_pwait(int64_t args[], int32_t *retval)
{
	int ret = 0;
	struct epoll_event *events; // This struct has same layout with args[1]
	int i;
	char __user *buf = (char __user *)args[1];
#ifdef CONFIG_POPCORN_DEBUG_MVX_SERVER
	int st_len = sizeof(struct epoll_event);
#endif

	/* No need to copy buffer. Epoll_pwait failed. */
	*retval = mvx_follower_msg.retval;
	if (*retval < 0) {
		if (*retval == -EINTR)	{// ctrl-c
			stop_mvx_process(current);
			do_exit(130);
		}
		else
			return;
	}
	/* Copy kernel buf to user space. */
	ret = copy_to_user(buf, mvx_follower_msg.buf, mvx_follower_msg.len);
	BUG_ON(ret);

	MVXPRINTK("-- len %d, sz_len %d\n", *retval, st_len);

	events = (struct epoll_event *)(mvx_follower_msg.buf);
	for (i = 0; i < *retval; i++) {
		MVXPRINTK("-- %2d: events 0x%x, data 0x%llx\n", i,
			  events[i].events, events[i].data);
	}

	MVXPRINTK("-- %s: ret %d, sizeof buf %u\n", __func__,
	       *retval, mvx_follower_msg.len);
	return;
}

/**
 * int accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen);
 * Cares about sockfd + retval.
 * */
static void follower_accept(int64_t args[], int32_t *retval)
{
	int sockfd = (int)args[0];
	//if (sockfd < 0) return -1;
	//fd_vtab[vtab_index].id = mvx_follower_msg.retval;
	//fd_vtab[vtab_index].real = 0;
	//vtab_index++;
	*retval = (int)(mvx_follower_msg.retval);
	if (*retval >= 0) {
		mvx_update_fd_vtab(*retval, MVX_SIM, MVX_FD_INC);
		sys_dup2(sockfd, *retval); // not sure whether it's correct
	}
	mvx_print_fd_vtab();
	MVXPRINTK("-- %s: orig fd %d, new fd %d\n", __func__, sockfd, *retval);
}

/**
 * flag from master indicate a real file (flag = 1);
 * real file: have to execute the syscall handler path.
 * */
static int follower_open(int64_t args[], int32_t *retval)
{
	if (mvx_follower_msg.flag == MVX_REAL) {
		return 1;
	} else {
		// TODO: seems all open files are real.
		*retval = (int)(mvx_follower_msg.retval);
		mvx_update_fd_vtab(*retval, MVX_SIM, MVX_FD_INC);
		mvx_print_fd_vtab();
		return 0;
	}

}

/**
 * int close(int fd);
 * Cares about fd + retval; real fd returns 1.
 * */
static int follower_close(int64_t args[], int32_t *retval)
{
#ifdef CONFIG_POPCORN_DEBUG_MVX_SERVER
	int fd = args[0];
#endif

	MVXPRINTK("%s: fd %d, is_real %d\n", __func__, fd, mvx_is_real_fd(fd));
//	mvx_print_fd_vtab();
	return 1;
//	if (mvx_is_real_fd(fd) || mvx_is_sim_close(fd)) return 1;
	// Handling the case of SIM fd, update fd_vtab here.
//	mvx_update_fd_vtab(fd, MVX_EMPTY, MVX_FD_DEC);
//	mvx_print_fd_vtab();
//	*retval = (int)(mvx_follower_msg.retval);
//	MVXPRINTK("-- %s: fd %d, ret %d\n", __func__, fd, *retval);
//	return 0;
}

/**
 * getsockopt only cares about the 3rd, 4th params, and retval.
 * int getsockopt(int sockfd, int level, int optname,
 *                       void *optval, socklen_t *optlen);
 * */
static void follower_getsockopt(int64_t args[], int32_t *retval)
{
	int ret;
	char __user *optval = (char __user *)args[3];
	int *optlen = (int *)args[4];	// optlen is also from userspace

	//*optlen = mvx_follower_msg.len;
	ret = copy_to_user(optlen, &(mvx_follower_msg.len), sizeof(int));
	BUG_ON(ret);
	//ret = copy_to_user(optval, mvx_follower_msg.buf, *optlen);
	ret = copy_to_user(optval, mvx_follower_msg.buf, mvx_follower_msg.len);
	BUG_ON(ret);
	*retval = (int32_t)(mvx_follower_msg.retval);
}

/**
 * offset + retval
 * ssize_t sendfile(int out_fd, int in_fd, off_t *offset, size_t count);
 * */
static void follower_sendfile(int64_t args[], int32_t *retval, int retsz)
{
	int ret;
	off_t __user *offset = (off_t __user *)args[2];

	ret = copy_to_user(offset, mvx_follower_msg.buf, mvx_follower_msg.len);
	BUG_ON(ret);

	if (retsz == 4)
		*retval = (uint32_t)mvx_follower_msg.retval;
	else
		*((uint64_t *)retval) = (uint64_t)mvx_follower_msg.retval;

	MVXPRINTK("%s: out_fd %lld, in_fd %lld. offset %ld, msg len %d. retval %ld\n",
		  __func__, args[0], args[1], *(off_t *)(mvx_follower_msg.buf),
		  mvx_follower_msg.len, mvx_follower_msg.retval);
}

/**
 * This is called in post syscall procedure.
 * Update retval, also send reply message.
 * */
static void follower_writev(int32_t *retval, unsigned int dst_nid)
{
	extern struct completion follower_wait;

	MVXPRINTK("\n");
	MVXPRINTK("Follower waits msg on\n[%2ld] syscall: <SYS_writev>\n",
		  mvx_index++);
	/* Follower waits for a message from master. */
	if (current->is_mvx_process)
		wait_for_completion_interruptible(&follower_wait);
	else
		goto out;

	MVX_WARN_ON(mvx_follower_msg.syscall != __NR_writev);

	MVXPRINTK("syscall %d: <%s>. flag %d. master msg syscall %d\n",
		  __NR_writev, syscall_name[__NR_writev],
		  mvx_follower_msg.flag, mvx_follower_msg.syscall);

	if (*retval != (ssize_t)mvx_follower_msg.retval)
		pr_info("follower ret %d != %ld from master\n",
			*retval, (ssize_t)mvx_follower_msg.retval);

	*(ssize_t *)retval = (ssize_t)mvx_follower_msg.retval;
	mvx_send_reply(*(ssize_t *)retval, __NR_writev, dst_nid);
out:
	return;
}

static void follower_write(int32_t *retval, unsigned int dst_nid)
{
	extern struct completion follower_wait;

	MVXPRINTK("\n");
	MVXPRINTK("Follower waits msg on\n[%2ld] syscall: <SYS_write>\n",
		  mvx_index++);
	/* Follower waits for a message from master. */
	if (current->is_mvx_process)
		wait_for_completion_interruptible(&follower_wait);
	else
		goto out;

	MVX_WARN_ON(mvx_follower_msg.syscall != __NR_write);

	MVXPRINTK("syscall %d: <%s>. flag %d. master msg syscall %d\n",
		  __NR_write, syscall_name[__NR_write],
		  mvx_follower_msg.flag, mvx_follower_msg.syscall);

	if (*retval != (ssize_t)mvx_follower_msg.retval)
		pr_info("follower ret %d != %ld from master\n",
			*retval, (ssize_t)mvx_follower_msg.retval);

	*(ssize_t *)retval = (ssize_t)mvx_follower_msg.retval;
	mvx_send_reply(*(ssize_t *)retval, __NR_write, dst_nid);
out:
	return;
}

/**
 * This is called by socket, epoll_create1; only update the retval.
 * So the retval is the simulated fd.
 * int socket(int domain, int type, int protocol);
 * */
static void follower_vtab_only(int32_t retval)
{
	if (retval >= 0) {
		mvx_update_fd_vtab(retval, MVX_SIM, MVX_FD_INC);
		mvx_print_fd_vtab();
		//fd_vtab[vtab_index].id = retval;
		//fd_vtab[vtab_index].real = 0;
		//vtab_index++;
		MVXPRINTK("-- %s: fd %d\n", __func__,
			  retval);
	}
}

static inline void follower_update_ret_only(int32_t *retval, int retsz)
{
	if (retsz == 4)
		*retval = (uint32_t)mvx_follower_msg.retval;
	else
		*((uint64_t *)retval) = (uint64_t)mvx_follower_msg.retval;
}

/**
 * MVX follower waits for master messages and executes syscalls.
 * return 1 indicates the follower needs to execute the syscall;
 * return 0: follower doesn't have to execute syscall.
 * */
int mvx_follower_wait_exec(struct task_struct *tsk, unsigned int dst_nid,
			   int syscall, int64_t args[], void *retval, int retsz)
{
	int ret = 0;
	extern struct completion follower_wait;

	MVXPRINTK("\n");
	MVXPRINTK("Follower waits msg on\n[%2ld] syscall %d: <%s>, retsz %d\n",
		  mvx_index++, syscall, syscall_name[syscall], retsz);
wait:
	/* Follower waits for a message. */
	if (current->is_mvx_process)
		wait_for_completion_interruptible(&follower_wait);
	else
		goto out;


	/* Corner case for master's extra epoll_pwait. */
	if (mvx_follower_msg.syscall == __NR_epoll_pwait) {
		if (syscall != __NR_epoll_pwait) {
			mvx_send_reply(0, syscall, dst_nid);
			goto wait;
		}
	}

	/* Corner case for read/recvfrom across different ISA. */
	if (syscall != __NR_recvfrom)
		MVX_WARN_ON(mvx_follower_msg.syscall != syscall);

	/* Follower simulates syscalls accordingly. */
	switch (syscall) {
	case __NR_read:		// 64 bit retval
	//case __NR_recvfrom:
		ret = follower_read(args, retval);
		break;
	case __NR_close:
		ret = follower_close(args, retval);
		break;
	case __NR_getsockopt:
		follower_getsockopt(args, retval);
		break;
	case __NR_sendfile:
		follower_sendfile(args, retval, retsz);
		break;
	case __NR_epoll_pwait:
		follower_epoll_pwait(args, retval);
		break;
#ifdef __x86_64__
	case __NR_open:
#endif
	case __NR_openat:
		ret = follower_open(args, retval);
		break;

	/* We have to update vtab and syscall retval. */
	case __NR_accept:
	case __NR_accept4:
		follower_accept(args, retval);
	/* Simulate syscalls update the return value only. */
	case __NR_setsockopt:
	case __NR_fcntl:	// 64 bit retval
	case __NR_epoll_ctl:
		follower_update_ret_only(retval, retsz);
		break;
	case __NR_exit_group:
	case __NR_exit:
		do_exit(args[0]); // TODO: may have bug here. should call sys_exit_group
		break;
	}

	/* Send ACK reply back to master. */
	if (retsz == 8)
		mvx_send_reply(*((int64_t *)retval), syscall, dst_nid);
	if (retsz == 4)
		mvx_send_reply(*((int32_t *)retval), syscall, dst_nid);
out:
	return ret;
}

/**
 * MVX follower executes after a syscall
 * */
int mvx_follower_post_syscall(struct task_struct *tsk, unsigned int dst_nid,
			   int syscall, int64_t args[], int *retval)
{
	switch (syscall) {
	/* These 2 syscall only in post procedure, update vtab only. */
	case __NR_epoll_create1:
	case __NR_socket:
		MVXPRINTK("\n[%2ld] syscall %d: <%s>\n", mvx_index++, syscall,
			  syscall_name[syscall]);
		follower_vtab_only(*retval);
		//WARN_ON(mvx_follower_msg.syscall != syscall);
		break;
	/* This syscall only in post, but have to send reply. */
	case __NR_writev:
		follower_writev(retval, dst_nid);
		break;
	case __NR_write:
		follower_write(retval, dst_nid);
		break;

	/* These 3 syscall was in pre procedure. */
	case __NR_openat:
		if (syscall == __NR_openat)
			mvx_update_fd_vtab(*retval, MVX_REAL, MVX_FD_INC);
#ifdef __x86_64__
	case __NR_open:
		if (syscall == __NR_open)
			mvx_update_fd_vtab(*retval, MVX_REAL, MVX_FD_INC);
#endif
	case __NR_close:
		if (syscall == __NR_close) {
			mvx_update_fd_vtab(args[0], MVX_EMPTY, MVX_FD_DEC);
			MVXPRINTK("%s: retval %d, maste ret %d\n", __func__,
				  *retval, (int)(mvx_follower_msg.retval));
			//*retval = (int)(mvx_follower_msg.retval);
		}
		mvx_print_fd_vtab();
	case __NR_read:
		//MVXPRINTK_POST("%s: ret %d\n", __func__, *retval);
		break;
	}

	return 0;
}

//==================================================================
//======================== Master routines =========================
/**
 * Read data from a fd; Copy user buffer (args[0]) to kernel message
 * buffer (msg->buf). The retval is the actual copied data size.
 *   ssize_t read(int fd, void *buf, size_t count);
 *   ssize_t recvfrom(int sockfd, void *buf, size_t len, int flags,
 *		      struct sockaddr *src_addr, socklen_t *addrlen);
 * */
static void master_read(mvx_message_t *msg, int64_t args[], int64_t retval)
{
	size_t ret = 0;
	char __user *buf = (char __user *)args[1];

	msg->len = 0;
	msg->retval = retval;
	msg->syscall = syscall_tbl[__NR_read];

	if (mvx_is_real_fd(args[0])) {
		// real desc, flag = MVX_REAL (1)
		msg->flag = MVX_REAL;
	} else {
		// not real desc. flag = MVX_SIM (2);
		// if it opens a fd, copy content to the buf
		msg->flag = MVX_SIM;
		if (retval > 0) {
			ret = copy_from_user(msg->buf, buf, retval);
			BUG_ON(ret);
			msg->len = retval;
			msg->buf[retval] = 0;
		}
	}
	MVXPRINTK("%s: fd %lld, flag %d, len %d, retval %lld.\n",
		  __func__, args[0], msg->flag, msg->len, retval);
}

/**
 * This is a tricky syscall, since struct epoll_event has 12 bytes on x86 and
 * 16 bytes on arm64. We want to handle the alignment issue on master side.
 * */
static void master_epoll_pwait(mvx_message_t *msg, int64_t args[],
			       int64_t retval)
{
	size_t ret = 0;
	int i;
	size_t events_len, events_dst_len;
	struct epoll_event *events; // This struct has same layout with args[1]

#ifdef __x86_64__  // x86_64 as master, send epoll_event in arm64 format.
	struct epoll_event_arm64 *events_dst
		= (struct epoll_event_arm64 *)(msg->buf);
	events_dst_len = sizeof(struct epoll_event_arm64) * retval;
#endif
#ifdef __aarch64__ // aarch64 as master, send epoll_event in x86_64 format.
	struct epoll_event_x86 *events_dst
		= (struct epoll_event_x86 *)(msg->buf);
	events_dst_len = sizeof(struct epoll_event_x86) * retval;
#endif
	/* Ctrl-C will cause a negative retval: no need to copy epoll_event. */
	if (retval < 0) {
		msg->len = 0;
		msg->retval = retval;
		msg->syscall = syscall_tbl[__NR_epoll_pwait];
		if (retval == -EINTR) stop_mvx_process(current);
		return;
	}

	/* Copy the epoll_event array to the kernel. */
	events_len = sizeof(struct epoll_event) * retval;
	events = (struct epoll_event *)kmalloc(events_len, GFP_KERNEL);
	ret = copy_from_user((void *)events, (void __user *)(args[1]), events_len);
	if (ret) MVXPRINTK("copy_from_user ret %ld\n", ret);
	BUG_ON(ret);

	/* Do the ISA-dependent data structure conversion. Copy to msg buf. */
	for (i = 0; i < retval; i++) {
		events_dst[i].events = events[i].events;
		events_dst[i].data = events[i].data;
		MVXPRINTK("-- %2d: events 0x%x, data 0x%llx\n", i,
			  events[i].events, events[i].data);
	}
	msg->len = events_dst_len;
	msg->retval = retval;
	msg->syscall = syscall_tbl[__NR_epoll_pwait];

	kfree(events);

	MVXPRINTK("-- %s: [arch dep] epoll_event %lu, dst len %ld, ret %lld\n",
	       __func__, sizeof(struct epoll_event), events_dst_len, retval);
	return;
}

static void master_accept(mvx_message_t *msg, int64_t args[], int64_t retval)
{
	if (retval >= 0) {
		mvx_update_fd_vtab(retval, MVX_SIM, MVX_FD_INC);
		mvx_print_fd_vtab();
	}
}

/**
 * Syscalls that operate on "virtual descriptors" only.
 * */
static void master_vtab_only(int64_t retval)
{
	if (retval >= 0) {
		mvx_update_fd_vtab(retval, MVX_SIM, MVX_FD_INC);
		MVXPRINTK("-- %s: fd %lld.\n", __func__, retval);
		mvx_print_fd_vtab();
	}
}

/**
 * Master deals with open/openat.
 * Open file in white list: a "real file" needs open on local node.
 *   int open(const char *pathname, int flags, mode_t mode);
 *   int openat(int dirfd, const char *pathname, int flags, mode_t mode);
 * */
static void master_open(mvx_message_t *msg, int syscall,
		    int64_t args[], int64_t retval)
{
	//char __user *buf = NULL;
	char *buf = NULL;

	/* pathname is the 1st param for open, and 2nd for openat. */
#ifdef __x86_64__
	if (syscall == __NR_open)
		buf = (char *)args[0];
		//buf = (char __user *)args[0];
#endif
	if (syscall == __NR_openat)
		buf = (char *)args[1];
		//buf = (char __user *)args[1];

	MVXPRINTK("%s: ret %lld. buf %p: %s\n",
		  __func__, retval, buf, buf);

	if (retval >= 0) {
		mvx_update_fd_vtab(retval, MVX_REAL, MVX_FD_INC);
		mvx_print_fd_vtab();
	}

	msg->syscall = syscall_tbl[syscall];
	msg->flag = MVX_REAL;	// Suppose all open fds are real.
	msg->len = 0;
	msg->retval = retval;
	msg->buf[0] = 0;
}

/**
 * Close only sends the "retval"; we have to modify fd_vtab only if retval==0.
 * But follower should only care args[0] (fd)
 * */
static void master_close(mvx_message_t *msg, int64_t args[], int64_t retval)
{
	if (retval == 0) {
		int closefd = args[0];
		mvx_update_fd_vtab(closefd, MVX_EMPTY, MVX_FD_DEC);
		mvx_print_fd_vtab();
	}
}

/**
 * int getsockopt(int sockfd, int level, int optname,
 *                void *optval, socklen_t *optlen);
 * optval + optlen + retval
 * */
static void master_getsockopt(mvx_message_t *msg, int syscall,
		    int64_t args[], int64_t retval)
{
	int ret;
	char __user *optval = (char __user *)args[3];
	int optlen = *((int __user *)args[4]);

	msg->len = optlen;
	msg->retval = retval;
	msg->syscall = syscall_tbl[__NR_getsockopt];
	ret = copy_from_user(msg->buf, optval, optlen);
	BUG_ON(ret);
	MVXPRINTK("%s: optlen %d, optval 0x%x. retval %lld.\n", __func__,
		  optlen, *(int *)optval, retval);
}

/**
 * ssize_t sendfile(int out_fd, int in_fd, off_t *offset, size_t count);
 * Cares about 'offset + retval'
 * */
static void master_sendfile(mvx_message_t *msg, int syscall,
		    int64_t args[], int64_t retval)
{
	int ret;
	off_t __user *offset = (off_t __user *)args[2];

	ret = copy_from_user(msg->buf, offset, sizeof(off_t));
	BUG_ON(ret);
	msg->len = sizeof(off_t);
	msg->retval = retval;
	msg->flag = 0;
	msg->syscall = syscall_tbl[__NR_sendfile];
	MVXPRINTK("%s: out_fd %lld, in_fd %lld, retval %lld\n",
		  __func__, args[0], args[1], retval);
}

/* Only update the retval. */
static void master_syscall_ret_only(mvx_message_t *msg, int dst_syscall_id,
				    int64_t retval)
{
	msg->syscall = dst_syscall_id;
	msg->len = 0;
	msg->retval = retval;
}

/**
 * The MVX request dispatcher function.
 * Master always executes this handler after the syscall.
 * */
int mvx_master_sync(struct task_struct *tsk, unsigned int dst_nid,
		    int syscall, int64_t args[], int64_t retval)
{
	int ret = 0;
	mvx_message_t *msg;
	extern struct completion master_wait;

	MVXPRINTK("\n[%2ld] syscall %d: <%s>\n", mvx_index++, syscall,
		  syscall_name[syscall]);
	/* Handle desc creation by operating VDT. NO need to send msg or wait ack. */
	switch (syscall) {
	case __NR_socket:
	case __NR_epoll_create1:
		master_vtab_only(retval);
		goto out;
	}

	/* Create a message with master syscall events. */
	msg = pcn_kmsg_get(sizeof(*msg));
	if (!msg) {
		ret = -ENOMEM;
		goto out;
	}

	switch (syscall) {
	case __NR_read:	    // arm64 recvfrom; x86 read??
	case __NR_recvfrom:
		master_read(msg, args, retval);
		break;
	case __NR_epoll_pwait:
		master_epoll_pwait(msg, args, retval);
		break;
	case __NR_getsockopt:
		master_getsockopt(msg, syscall, args, retval);
		break;
	case __NR_sendfile:
		master_sendfile(msg, syscall, args, retval);
		break;
#ifdef __x86_64__
	case __NR_open:
#endif
	case __NR_openat:
		master_open(msg, syscall, args, retval);
		break;

	/* Master updates vdtab and forwards syscall retval. */
	case __NR_accept:
	case __NR_accept4:
		if (syscall == __NR_accept || syscall == __NR_accept4) {//TODO: remove it?
			master_accept(msg, args, retval);
		}
	case __NR_close:
		if (syscall == __NR_close) {
			master_close(msg, args, retval);
		}
	case __NR_writev:
	case __NR_write:
		if (syscall == __NR_writev || syscall == __NR_write) {
			if (mvx_is_real_fd(args[0])) {
				msg->flag = MVX_REAL;
			} else {
				msg->flag = MVX_SIM;
			}
		}
	/* Master uses retval only. */
	case __NR_setsockopt:
	case __NR_fcntl:
	case __NR_epoll_ctl:
	case __NR_exit_group:	// not a traditional ret only syscall.
	case __NR_exit:
		master_syscall_ret_only(msg, syscall_tbl[syscall], retval);
		break;
	}

	/* Send message, wait reply, and delete the dynamic allocated msg. */
	ret = pcn_kmsg_post(PCN_KMSG_TYPE_MVX_MSG, dst_nid, msg, sizeof(*msg));
	MVXPRINTK("master waits for follower return on syscall %d\n", syscall);
	if (syscall != __NR_exit_group)
		wait_for_completion_interruptible(&master_wait);
	//pcn_kmsg_done(msg); // for safety purpose, I deleted the msg here.
out:
	return ret;
}
