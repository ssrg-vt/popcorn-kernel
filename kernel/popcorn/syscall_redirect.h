/**
* Header file for Popcorn system call redirection
*
*     Ashwin Krishnakumar <kashwin@vt.edu> 2020
*/

#ifdef SYSCALL_REDIRECT_H
#define SYSCALL_REDIRECT_H

#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/sched/task_stack.h>
#include <linux/mm.h> 
#include <linux/smp.h>
#include <linux/errno.h>
#include <linux/ptrace.h>
#include <linux/tracehook.h>
#include <linux/audit.h>
#include <linux/seccomp.h>
#include <linux/signal.h>
#include <linux/export.h>
#include <linux/context_tracking.h>
#include <linux/user-return-notifier.h>
#include <linux/nospec.h>
#include <linux/uprobes.h>
#include <linux/livepatch.h>
#include <linux/syscalls.h>
#include <linux/uaccess.h>

extern long syscall_redirect(unsigned long nr, struct pt_regs *regs);
 int handle_signal_remotes(struct pcn_kmsg_message  *msg);
extern const int redirect_table[];
int process_remote_syscall(struct pcn_kmsg_message *msg);
/*= {__NR_socket , __NR_setsockopt , __NR_bind ,
			      __NR_listen , __NR_accept4 , __NR_shutdown,
			      __NR_recvfrom, __NR_epoll_create1 , __NR_epoll_wait, 
			     __NR_epoll_pwait , __NR_epoll_ctl , __NR_read ,
			     __NR_write , __NR_open , __NR_close , __NR_ioctl,
                             __NR_writev , __NR_fstat , __NR_sendfile , __NR_select,
                	     __NR_fcntl , __NR_fstat , __NR_getpid  } ;
*/
#endif
