#include<linux/module.h>
#include<linux/version.h>
#include<linux/kernel.h>
#include<linux/init.h>
#include<linux/kprobes.h>
#include<linux/syscalls.h>
#include <linux/fs.h>
#include <linux/file.h>

#include "syscall_redirect.h"
#include "wait_station.h"
#include "types.h"

extern void do_syscall_64(unsigned long nr, struct pt_regs *regs);

#ifdef CONFIG_X86_32
void syscall_get_arg(struct task_struct *task,
                                         struct pt_regs *regs,
                                         unsigned long *args)
{
        memcpy(args, &regs->bx, 6 * sizeof(args[0]));
}

syscall_set_arg(struct task_struct *task,
                                         struct pt_regs *regs,
                                         unsigned int i, unsigned int n,
                                         const unsigned long *args)
{
        BUG_ON(i + n > 6); 
        memcpy(&regs->bx + i, args, n * sizeof(args[0]));
}


#else

void syscall_get_arg(struct task_struct *task,
                                         struct pt_regs *regs,
                                         unsigned long *args)
{
# ifdef CONFIG_IA32_EMULATION
        if (task->thread_info.status & TS_COMPAT) {
                *args++ = regs->bx;
                *args++ = regs->cx;
                *args++ = regs->dx;
                *args++ = regs->si;
                *args++ = regs->di;
                *args   = regs->bp;
        } else
# endif
        {
                *args++ = regs->di;
                *args++ = regs->si;
                *args++ = regs->dx;
                *args++ = regs->r10;
                *args++ = regs->r8;
                *args   = regs->r9;
        }
}

void syscall_set_arg(struct task_struct *task,
                                         struct pt_regs *regs,
                                         const unsigned long *args)
{
# ifdef CONFIG_IA32_EMULATION
        if (task->thread_info.status & TS_COMPAT) {
                regs->bx = *args++;
                regs->cx = *args++;
                regs->dx = *args++;
                regs->si = *args++;
                regs->di = *args++;
                regs->bp = *args;
        } else
# endif
        {
                regs->di = *args++;
                regs->si = *args++;
                regs->dx = *args++;
                regs->r10 = *args++;
                regs->r8 = *args++;
                regs->r9 = *args;
        }
}

#endif

int process_remote_syscall(struct pcn_kmsg_message *msg)
{
	int retval,temp;
	int (* remote_syscall)(struct pt_regs * ) ;
	syscall_fwd_t *req = (syscall_fwd_t *) msg;
	syscall_rep_t *rep = pcn_kmsg_get(sizeof(*rep));
	struct pt_regs reg; 
	printk(KERN_INFO "remote system call num %d received at index %d\n\n "  \
		,redirect_table[req->call_type],req->call_type);
	syscall_set_arg(current,&reg,(unsigned long *)&req->args);	

	printk("Parameters are %x \n%x \n%x \n%x \n%x \n%x\n",req->args[0],req->args[1],req->args[2],req->args[3],req->args[4],req->args[5]);	

	do_syscall_64(redirect_table[req->call_type] ,&reg);	
	retval = reg.ax;	
	rep->origin_pid = current->origin_pid;
	rep->remote_ws = req->remote_ws;
	rep->ret = retval;
	pcn_kmsg_post(PCN_KMSG_TYPE_SYSCALL_REP, 
		current->remote_nid, rep,sizeof(*rep));
	pcn_kmsg_done(req);
	printk(KERN_INFO"Return value from master %d\n\n for syscall \
		number %d ",retval,redirect_table[req->call_type]);
	return retval;

		
}
