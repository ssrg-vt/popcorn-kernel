/*
 * File:
 * 	process_server.c
 *
 * Description:
 * 	this file implements the x86 architecture specific
 *  helper functionality of the process server
 *
 * Created on:
 * 	Sep 19, 2014
 *
 * Author:
 * 	Sharath Kumar Bhat, SSRG, VirginiaTech
 *
 */

/* File includes */
#include <linux/sched.h>
#include <linux/kdebug.h>
#include <linux/ptrace.h>
#include <asm/uaccess.h>
#include <asm/prctl.h>
#include <asm/proto.h>
#include <asm/desc.h>
#include <asm/fpu/internal.h>

#include <popcorn/types.h>
#include <popcorn/regset.h>

/*
 * Function:
 *		save_thread_info
 *
 * Description:
 *		this function saves the architecture specific info of the task
 *		to the struct struct field_arch structure passed
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
	unsigned short fsindex, gsindex;
	unsigned long ds, es, fs, gs;
	int cpu;

	BUG_ON(!arch);

	cpu = get_cpu();

	/*
	 * Segments
	 * CS and SS are set during the user/kernel mode switch.
	 * Thus, nothing to do with them.
	 */

	ds = current->thread.ds;
	es = current->thread.es;

	savesegment(fs, fsindex);
	if (fsindex) {
		fs = get_desc_base(current->thread.tls_array + current->thread.fsbase);
	} else {
		rdmsrl(MSR_FS_BASE, fs);
	}

	savesegment(gs, gsindex);
	if (gsindex) {
		gs = get_desc_base(current->thread.tls_array + current->thread.gsbase);
	} else {
		rdmsrl(MSR_KERNEL_GS_BASE, gs);
	}

	WARN_ON(ds);
	WARN_ON(es);
	WARN_ON(gs);
	arch->tls = fs;

	put_cpu();

	PSPRINTK("%s [%d] tls %lx\n", __func__, current->pid, arch->tls);

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
 *	restore_segments,
 *			restore segmentations as well if segs is true. Unless, do
 *			not restore the segmentation units (for back migration)
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
	struct regset_x86_64 *regset = &arch->regs_x86;
	int cpu;

	cpu = get_cpu();

	regs->r15 = regset->r15;
	regs->r14 = regset->r14;
	regs->r13 = regset->r13;
	regs->r12 = regset->r12;
	regs->bp = regset->rbp;
	regs->bx = regset->rbx;

	regs->r11 = regset->r11;
	regs->r10 = regset->r10;
	regs->r9 = regset->r9;
	regs->r8 = regset->r8;
	regs->ax = regset->rax;
	regs->cx = regset->rcx;
	regs->dx = regset->rdx;
	regs->si = regset->rsi;
	regs->di = regset->rdi;

	regs->ip = regset->rip;
	regs->sp = regset->rsp;
	regs->flags = regset->rflags;

	if (restore_segments) {
		regs->cs = __USER_CS;
		regs->ss = __USER_DS;

		/*
		current->thread.ds = regset->ds;
		current->thread.es = regset->es;
		*/

		if (arch->tls) {
			do_arch_prctl_64(current, ARCH_SET_FS, arch->tls);
		}
		/*
		if (arch->thread_gs) {
			do_arch_prctl_64(current, ARCH_SET_GS, arch->thread_gs);
		}
		*/
	}

	put_cpu();

#ifdef CONFIG_POPCORN_DEBUG_VERBOSE
	PSPRINTK("%s [%d] ip %lx\n", __func__, current->pid,
			regs->ip);
	PSPRINTK("%s [%d] sp %lx bp %lx\n", __func__, current->pid,
			regs->sp, regs->bp);
#endif
	return 0;
}

#include <asm/stacktrace.h>
noinline_for_stack void update_frame_pointer(void)
{
	unsigned long *rbp;
	rbp = __builtin_frame_address(0); /* update_frame_pointer */

	/* User rbp is at one stack frames below */
	*rbp = current_pt_regs()->bp;	/* sched_migrate */
}


/*
 * Function:
 *		dump_processor_regs
 *
 * Description:
 *		this function prints the architecture specific registers specified
 *		in the input argument
 *
 * Input:
 * 	task,	pointer to the architecture specific registers
 *
 * Output:
 * 	none
 *
 * Return value:
 *	void
 *
 * Why don't use show_all() for x86?
 */
void dump_processor_regs(struct pt_regs* regs)
{
	unsigned long fs, gs;
	unsigned long fsindex, gsindex;

	dump_stack();
	if (!regs) return;
	printk(KERN_ALERT"DUMP REGS %s\n", __func__);

	if (NULL != regs) {
		printk(KERN_ALERT"r15{%lx}\n",regs->r15);
		printk(KERN_ALERT"r14{%lx}\n",regs->r14);
		printk(KERN_ALERT"r13{%lx}\n",regs->r13);
		printk(KERN_ALERT"r12{%lx}\n",regs->r12);
		printk(KERN_ALERT"r11{%lx}\n",regs->r11);
		printk(KERN_ALERT"r10{%lx}\n",regs->r10);
		printk(KERN_ALERT"r9{%lx}\n",regs->r9);
		printk(KERN_ALERT"r8{%lx}\n",regs->r8);
		printk(KERN_ALERT"bp{%lx}\n",regs->bp);
		printk(KERN_ALERT"bx{%lx}\n",regs->bx);
		printk(KERN_ALERT"ax{%lx}\n",regs->ax);
		printk(KERN_ALERT"cx{%lx}\n",regs->cx);
		printk(KERN_ALERT"dx{%lx}\n",regs->dx);
		printk(KERN_ALERT"di{%lx}\n",regs->di);
		printk(KERN_ALERT"orig_ax{%lx}\n",regs->orig_ax);
		printk(KERN_ALERT"ip{%lx}\n",regs->ip);
		printk(KERN_ALERT"cs{%lx}\n",regs->cs);
		printk(KERN_ALERT"flags{%lx}\n",regs->flags);
		printk(KERN_ALERT"sp{%lx}\n",regs->sp);
		printk(KERN_ALERT"ss{%lx}\n",regs->ss);
	}
	rdmsrl(MSR_FS_BASE, fs);
	rdmsrl(MSR_GS_BASE, gs);
	printk(KERN_ALERT"fs{%lx} - %lx content %lx\n",fs, current->thread.fsbase, fs ? * (unsigned long*)fs : 0x1234567l);
	printk(KERN_ALERT"gs{%lx} - %lx content %lx\n",gs, current->thread.gsbase, fs ? * (unsigned long*)gs : 0x1234567l);

	savesegment(fs, fsindex);
	savesegment(gs, gsindex);
	printk(KERN_ALERT"fsindex{%lx} - %x\n",fsindex, current->thread.fsindex);
	printk(KERN_ALERT"gsindex{%lx} - %x\n",gsindex, current->thread.gsindex);
	printk(KERN_ALERT"REGS DUMP COMPLETE\n");
}
