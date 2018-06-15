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
int save_thread_info(struct field_arch *arch)
{
	int cpu;
	struct pt_regs *regs = current_pt_regs();

	cpu = get_cpu();

	/* TODO handle these registers at the userspace correctly */
	arch->oob[0] = regs->msr;
	arch->oob[1] = regs->xer;

	/* TODO set arch->tls and arch->fpu_active */
	arch->tls = 0;
	arch->fpu_active = false;

	put_cpu();

	PSPRINTK("%s [%d] ip %lx lr %lx\n", __func__, current->pid,
			instruction_pointer(regs), regs->link);
	/*
	PSPRINTK("%s [%d] tls %lx\n", __func__, current->pid, arch->tls);
	PSPRINTK("%s [%d] fpu %sactive\n", __func__, current->pid,
			arch->fpu_active ? "" : "in");
	*/

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
int restore_thread_info(struct field_arch *arch, bool restore_segments)
{
	struct pt_regs *regs = current_pt_regs();
	struct regset_powerpc *regset = &arch->regs_ppc;
	int cpu, i;

	cpu = get_cpu();

	regs->nip = regset->nip;
	regs->link = regset->link;
	regs->ctr = regset->ctr;
	regs->ccr = regset->ccr;

	regs->msr = arch->oob[0];
	regs->xer = arch->oob[1];

	for (i = 0; i < 32; i++) {
		regs->gpr[i] = regset->gpr[i];
	}

	if (restore_segments) {
		/* TODO set up TLS and FPU status */
	}
	put_cpu();

	PSPRINTK("%s [%d] ip %lx lr %lx\n", __func__, current->pid,
			instruction_pointer(regs), regs->link);
	//show_regs(regs);

	return 0;
}


noinline_for_stack void update_frame_pointer(void)
{
	/*
	unsigned long *lr;
	asm volatile ("mtlr 4; std 4, %0" : "=m"(lr));

	*lr = current_pt_regs()->link;
	*/
}
