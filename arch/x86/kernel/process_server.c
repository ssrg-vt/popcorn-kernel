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
#include <linux/cpu_namespace.h>
#include <linux/kdebug.h>
#include <linux/ptrace.h>
#include <asm/uaccess.h>
#include <asm/prctl.h>
#include <asm/proto.h>
#include <asm/desc.h>

#include <popcorn/types.h>
#include <popcorn/debug.h>

/*
 * Function:
 *		save_thread_info
 *
 * Description:
 *		this function saves the architecture specific info of the task
 *		to the field_arch structure passed
 *
 * Input:
 *	task,	pointer to the task structure of the task of which the
 *			architecture specific info needs to be saved
 *
 *	regs,	pointer to the pt_regs field of the task
 *
 * Output:
 *	arch,	pointer to the field_arch structure type where the
 *			architecture specific information of the task has to be
 *			saved
 *
 * Return value:
 *	on success, returns 0
 * 	on failure, returns negative integer
 */
int save_thread_info(struct task_struct *tsk, field_arch *arch)
{
	unsigned short fsindex, gsindex;
	unsigned long ds, es, fs, gs;
	int cpu;

	BUG_ON(!tsk || !arch);
	BUG_ON(current != tsk);

	cpu = get_cpu();

	/*
	 * Segments
	 * CS and SS are set during the user/kernel mode switch.
	 * Thus, nothing to do with them.
	 */

	ds = tsk->thread.ds; // 0 usually
	es = tsk->thread.es; // 0 usually

	savesegment(fs, fsindex);
	if (fsindex) {
		fs = get_desc_base(tsk->thread.tls_array + FS_TLS);
	} else {
		rdmsrl(MSR_FS_BASE, fs);
	}

	savesegment(gs, gsindex);
	if (gsindex) {
		gs = get_desc_base(tsk->thread.tls_array + GS_TLS);
	} else {
		rdmsrl(MSR_KERNEL_GS_BASE, gs);
	}

	arch->tls = fs;

#ifdef MIGRATE_FPU
	save_fpu_info(tsk, arch);
#endif
	put_cpu();

	/*
	PSPRINTK(KERN_INFO"%s [%d]: ip %lx\n", __func__,
			tsk->pid, arch->ip);
	PSPRINTK(KERN_INFO"%s [%d]: sp %lx bp %lx\n", __func__,
			tsk->pid, arch->sp, arch->bp);
	*/
	PSPRINTK(KERN_INFO"%s [%d]: tls %lx gs %lx\n", __func__,
			tsk->pid, arch->tls, gs);

	return 0;
}


/*
 * Function:
 *		restore_thread_info
 *
 * Description:
 *		this function restores the architecture specific info of the
 *		task from the field_arch structure passed
 *
 * Input:
 * 	tsk,	pointer to the task structure of the task of which the
 * 			architecture specific info needs to be restored
 *
 * 	arch,	pointer to the field_arch structure type from which the
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
int restore_thread_info(struct task_struct *tsk, field_arch *arch, bool restore_segments)
{
	struct pt_regs *regs = task_pt_regs(tsk);
	struct regset_x86_64 *regset = &arch->regs_x86;
	int cpu;

	BUG_ON(restore_segments && current != tsk);

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

		tsk->thread.ds = regset->ds;
		tsk->thread.es = regset->es;

		if (arch->tls) {
			do_arch_prctl(tsk, ARCH_SET_FS, arch->tls);
		}
		/*
		if (arch->thread_gs) {
			do_arch_prctl(tsk, ARCH_SET_GS, arch->thread_gs);
		}
		*/
		printk("%lx %x %x\n", arch->tls, regset->fs, regset->gs);
	}
	//initialize_thread_retval(tsk, 0);

#ifdef MIGRATE_FPU
	restore_fpu_info(tsk, arch);
#endif
	put_cpu();

	PSPRINTK(KERN_INFO"%s [%d]: ip %lx\n", __func__,
			tsk->pid, regs->ip);
	PSPRINTK(KERN_INFO"%s [%d]: sp %lx bp %lx\n", __func__,
			tsk->pid, regs->sp, regs->bp);
	PSPRINTK(KERN_INFO"%s [%d]: fs %lx\n", __func__,
			tsk->pid, arch->tls);

	return 0;
}


/*
 * Function:
 *		update_thread_info
 *
 * Description:
 *		this function updates the task's thread structure to
 *		the latest register values.
 *
 * Input:
 * 	task,	pointer to the task structure of the task of which the
 * 			thread structure needs to be updated
 *
 * Output:
 * 	none
 *
 * Return value:
 *	on success, returns 0
 * 	on failure, returns negative integer
 */
int update_thread_info(field_arch *arch)
{
#ifdef MIGRATE_FPU
	update_fpu_info(current);
#endif

	return 0;
}

/*
 * Function:
 *		initialize_thread_retval
 *
 * Description:
 *		this function sets the return value of the task
 *		to the value specified in the argument val
 *
 * Input:
 * 	task,	pointer to the task structure of the task of which the
 * 			return value needs to be set
 * 	val,	the return value to be set
 *
 * Output:
 * 	none
 *
 * Return value:
 *	on success, returns 0
 * 	on failure, returns negative integer
 */
int initialize_thread_retval(struct task_struct *tsk, int val)
{
	//printk("%s [+] TID: %d\n", __func__, tsk->pid);
	BUG_ON(!tsk);

	task_pt_regs(tsk)->ax = val;
	//printk("%s [-] TID: %d\n", __func__, tsk->pid);

	return 0;
}


#ifdef MIGRATE_FPU
/*/
 * Function:
 *		save_fpu_info
 *
 * Description:
 *		this function saves the FPU info of the task specified
 *		to the arch structure specified in the argument
 *
 * Input:
 * 	task,	pointer to the task structure of the task of which the
 * 			FPU info needs to be saved
 *
 * Output:
 * 	arch,	pointer to the field_arch structure where the FPU info
 * 			needs to be saved
 *
 * Return value:
 *	on success, returns 0
 * 	on failure, returns negative integer
 */
int save_fpu_info(struct task_struct *tsk, field_arch *arch)
{
	struct fpu temp;

	//printk("%s [+] TID: %d\n", __func__, tsk->pid);
	if ((tsk == NULL)  || (arch == NULL)) {
		printk(KERN_ERR"process_server: invalid params to %s", __func__);
		return -EINVAL;
	}

	//FPU migration code --- initiator
	//PSPRINTK(KERN_ERR "%s: task flags %x fpu_counter %x has_fpu %x [%d:%d] %d:%d %x\n",
	//		__func__, tsk->flags, (int)tsk->fpu_counter, (int)tsk->thread.has_fpu,
	//		(int)__thread_has_fpu(tsk), (int)fpu_allocated(&tsk->thread.fpu),
	//		(int)use_xsave(), (int)use_fxsr(), (int) PF_USED_MATH);

	arch->task_flags = tsk->flags;
	arch->task_fpu_counter = tsk->thread.fpu.counter;
	arch->thread_has_fpu = tsk->thread.fpu.fpregs_active;

	//    if (__thread_has_fpu(tsk)) {
	if (!fpu_allocated(&tsk->thread.fpu)){
		fpu_alloc(&tsk->thread.fpu);
		fpu_finit(&tsk->thread.fpu);
	}

	fpu_save_init(&tsk->thread.fpu);

	temp.state = &request->fpu_state;
	fpu_copy(&temp,&tsk->thread.fpu);
	//printk("%s [-] TID: %d\n", __func__, tsk->pid);

	return 0;
}

/*
 * Function:
 *		restore_fpu_info
 *
 * Description:
 *		this function restores the FPU info of the task specified
 *		from the arch structure specified in the argument
 *
 * Input:
 * 	task,	pointer to the task structure of the task of which the
 * 			FPU info needs to be restored
 *
 * 	arch,	pointer to the field_arch struture from where the fpu info
 * 			needs to be restored
 *
 * Output:
 * 	none
 *
 * Return value:
 *	on success, returns 0
 * 	on failure, returns negative integer
 */
int restore_fpu_info(struct task_struct *tsk, field_arch *arch)
{
	struct fpu fpu;
	//printk("%s [+] TID: %d\n", __func__, tsk->pid);
	BUG_ON(!tsk || !arch);

	//FPU migration code --- server
	/* PF_USED_MATH is set if the task used the FPU before
	 * fpu_counter is incremented every time you go in __switch_to while owning the FPU
	 * has_fpu is true if the task is the owner of the FPU, thus the FPU contains its data
	 * fpu.preload (see arch/x86/include/asm.i387.h:switch_fpu_prepare()) is a heuristic
	 */
	if (arch->task_flags & PF_USED_MATH)
	//set_used_math();
	set_stopped_child_used_math(tsk);

	tsk->fpu_counter = arch->task_fpu_counter;

	if (!fpu_allocated(&tsk->thread.fpu)) {
		fpu_alloc(&tsk->thread.fpu);
		fpu_finit(&tsk->thread.fpu);
	}

	fpu.state = &arch->fpu_state;
	fpu_copy(&tsk->thread.fpu, &fpu);

	//PSPRINTK(KERN_ERR "%s: task flags %x fpu_counter %x has_fpu %x [%d:%d]\n",
	//		__func__, tsk->flags, (int)tsk->fpu_counter, (int)tsk->thread.has_fpu,
	//		(int)__thread_has_fpu(tsk), (int)fpu_allocated(&tsk->thread.fpu));

	//FPU migration code --- is the following optional?
	if (tsk_used_math(tsk) && tsk->fpu_counter >5)//fpu.preload
	__math_state_restore(tsk);

	//printk("%s [-] TID: %d\n", __func__, tsk->pid);
	return 0;
}

/*
 * Function:
 *		update_fpu_info
 *
 * Description:
 *		this function updates the FPU info of the task specified
 *
 * Input:
 * 	task,	pointer to the task structure of the task of which the
 * 			FPU info needs to be updated
 *
 * Output:
 * 	none
 *
 * Return value:
 *	on success, returns 0
 * 	on failure, returns negative integer
 */
int update_fpu_info(struct task_struct *tsk)
{
	//printk("%s [+] TID: %d\n", __func__, tsk->pid);
	if (tsk == NULL){
		printk(KERN_ERR"process_server: invalid params to %s", __func__);
		return -EINVAL;
	}

	if (tsk_used_math(tsk) && tsk->fpu_counter >5) //fpu.preload
		__math_state_restore(tsk);
	//printk("%s [-] TID: %d\n", __func__, tsk->pid);

	return 0;
}

#endif	// MIGRATE_FPU


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
	printk(KERN_ALERT"fs{%lx} - %lx content %lx\n",fs, current->thread.fs, fs ? * (unsigned long*)fs : 0x1234567l);
	printk(KERN_ALERT"gs{%lx} - %lx content %lx\n",gs, current->thread.gs, fs ? * (unsigned long*)gs : 0x1234567l);

	savesegment(fs, fsindex);
	savesegment(gs, gsindex);
	printk(KERN_ALERT"fsindex{%lx} - %x\n",fsindex, current->thread.fsindex);
	printk(KERN_ALERT"gsindex{%lx} - %x\n",gsindex, current->thread.gsindex);
	printk(KERN_ALERT"REGS DUMP COMPLETE\n");
}
