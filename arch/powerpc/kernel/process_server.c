/*
 * File:
 * 	process_server.c
 *
 * Description:
 * 	Helper functionality of the process server
 *
 * Created on:
 * 	July 13, 2017
 *
 * Author:
 * 	Sang-Hoon Kim, SSRG, Virginia Tech
 *
 */

/* File includes */
#include <linux/sched.h>
#include <linux/uaccess.h>
#include <linux/ptrace.h>

#include <popcorn/regset.h>
#include <popcorn/debug.h>

/*
 * Function:
 *		save_thread_info
 *
 * Description:
 *		this function saves the architecture specific info of the task
 *		to the struct field_arch structure passed
 *
 * Input:
 *	task,	pointer to the task structure of the task of which the
 *			architecture specific info needs to be saved
 *
 *	regs,	pointer to the pt_regs field of the task
 *
 * Output:
 *	arch,	pointer to the struct field_arch structure type where the
 *			architecture specific information of the task has to be
 *			saved
 *
 * Return value:
 *	on success, returns 0
 * 	on failure, returns negative integer
 */
int save_thread_info(struct task_struct *task, struct field_arch *arch)
{
	int cpu;

	cpu = get_cpu();

	/* TODO set arch->tls and arch->fpu_active */

	put_cpu();

	PSPRINTK("%s [%d] tls %lx\n", __func__, task->pid, arch->tls);
	PSPRINTK("%s [%d] fpu %sactive\n", __func__, task->pid,
			arch->fpu_active ? "" : "in");

	return 0;
}

/*
 * Function:
 *		restore_thread_info
 *
 * Description:
 *		this function restores the architecture specific info of the
 *		task from the struct field_arch structure passed
 *
 * Input:
 * 	task,	pointer to the task structure of the task of which the
 * 			architecture specific info needs to be restored
 *
 * 	arch,	pointer to the struct field_arch structure type from which the
 *			architecture specific information of the task has to be
 *			restored
 *
 * Output:
 *	none
 *
 * Return value:
 *	on success, returns 0
 * 	on failure, returns negative integer
 */
int restore_thread_info(struct task_struct *task, struct field_arch *arch, bool restore_segments)
{
	struct pt_regs *regs = task_pt_regs(task);
	struct regset_powerpc *regset = &arch->regs_ppc;
	int cpu, i;

	BUG_ON(restore_segments && current != task);

	cpu = get_cpu();

	regs->nip = regset->nip;
	regs->msr = regset->msr;
	regs->orig_gpr3 = regset->orig_gpr3;
	regs->ctr = regset->ctr;
	regs->link = regset->link;
	regs->xer = regset->xer;
	regs->ccr = regset->ccr;

	for (i = 0; i < 31; i++)
		regs->gpr[i] =  regset->gpr[i];

	if (restore_segments) {
		/* TODO set up TLS and FPU status */
	}
	put_cpu();

	PSPRINTK("%s [%d] pc %lx sp %lx\n", __func__, task->pid,
			regs->nip, regs->ctr);
	show_regs(regs);

	return 0;
}


noinline_for_stack void update_frame_pointer(void)
{
}
