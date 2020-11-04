#include <linux/module.h>
#include <linux/version.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/kprobes.h>
#include <linux/syscalls.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <asm/ptrace.h>

#include "syscall_redirect.h"
#include "wait_station.h"
#include "types.h"

typedef long (*syscall_fn_t)(const struct pt_regs *regs);
extern const syscall_fn_t sys_call_table[];

void syscall_get_arg(struct task_struct *task,
		     struct pt_regs *regs,
		     unsigned long *args)
{
	args[0] = regs->orig_x0;
	args++;

	memcpy(args, &regs->regs[1], 5 * sizeof(args[0]));
}

void syscall_set_arg(struct task_struct *task,
		     struct pt_regs *regs,
		     const unsigned long *args)
{
	regs->orig_x0 = args[0];
	args++;

	memcpy(&regs->regs[1], args, 5 * sizeof(args[0]));
}

int process_remote_syscall(struct pcn_kmsg_message *msg)
{
	int retval,temp;
	int (* remote_syscall)(struct pt_regs * ) ;
	syscall_fwd_t *req = (syscall_fwd_t *) msg;
	syscall_rep_t *rep = pcn_kmsg_get(sizeof(*rep));
	struct pt_regs reg;

	printk(KERN_INFO "remote system call num %d received\n\n",
	       redirect_table[req->call_type]);

	syscall_set_arg(current,&reg,(unsigned long *)&req->args);

	const syscall_fn_t syscallfn =
		sys_call_table[redirect_table[req->call_type]];

	retval = syscallfn(&reg);

	rep->origin_pid = current->origin_pid;
	rep->remote_ws = req->remote_ws;
	rep->ret = retval;

	pcn_kmsg_post(PCN_KMSG_TYPE_SYSCALL_REP,
		current->remote_nid, rep,sizeof(*rep));

	pcn_kmsg_done(req);

	printk(KERN_INFO
	       "Return value from master %d\n\n for syscall number %d",
	       retval, redirect_table[req->call_type]);

	return retval;
}
