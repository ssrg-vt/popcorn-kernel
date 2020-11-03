/**
* Header file for Popcorn system call redirection
*
*     Ashwin Krishnakumar <kashwin@vt.edu> 2020
*/

#ifndef SYSCALL_REDIRECT_H
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
#include "types.h"

extern long syscall_redirect(unsigned long nr, struct pt_regs *regs);
int handle_signal_remotes(struct pcn_kmsg_message  *msg);
extern const int redirect_table[];
int process_remote_syscall(struct pcn_kmsg_message *msg);
void syscall_get_arg(struct task_struct *task,
		     struct pt_regs *regs,
		     unsigned long *args);
void syscall_set_arg(struct task_struct *task,
		     struct pt_regs *regs,
		     const unsigned long *args);
#endif
