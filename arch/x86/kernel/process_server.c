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
#include <linux/popcorn_cpuinfo.h>
#include <linux/process_server.h>
#if 0 // beowulf
#include <asm/i387.h>
#endif
#include <asm/uaccess.h>
#include <process_server_arch.h>

#ifdef PSPRINTK
#undef PSPRINTK
#define PSPRINTK(...)
#endif

/* External function declarations */
extern void __show_regs(struct pt_regs *regs, int all);
extern unsigned long read_old_rsp(void);
extern struct task_struct* do_fork_for_main_kernel_thread(unsigned long clone_flags,
		unsigned long stack_start, struct pt_regs *regs, unsigned long stack_size,
		int __user *parent_tidptr, int __user *child_tidptr);

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
int save_thread_info(struct task_struct *task, struct pt_regs *regs,
                     field_arch *arch, void __user *uregs)
{
	int ret;
	unsigned short fsindex, gsindex;
	unsigned short es, ds;
	unsigned long fs, gs;

	//dump_processor_regs(task_pt_regs(task));

	if ((task == NULL)  || (arch == NULL)) {
		printk(KERN_ERR"%s: invalid params\n", __func__);
		return -EINVAL;
	}

	if (task->migration_pc == 0) {
		printk(KERN_ERR"%s: migration_pc == 0\n", __func__);
		return -EINVAL;
	}

	if (uregs != NULL) {
		// TODO beowulf: aarch / x86
		ret = copy_from_user(&arch->regs_aarch, uregs, sizeof(struct popcorn_regset_aarch64));
		if (ret != 0) {
			printk(KERN_ERR"%s: ERROR: while copying registers (%d)\n", __func__, ret);
		}
	}

	arch->migration_pc = task->migration_pc;

	memcpy(&arch->regs, regs, sizeof(struct pt_regs));

	arch->thread_usersp = task->thread.usersp;
	task->saved_old_rsp = read_old_rsp();
	arch->old_rsp = read_old_rsp();

	/*
	 * Also save frame pointer and return address, required for stack
	 * transformation.
	 */
	arch->bp = regs->bp;
	arch->ra = task->return_addr;

	/* Segments */
	arch->thread_es = task->thread.es;
	savesegment(es, es);

	arch->thread_ds = task->thread.ds;
	savesegment(ds, ds);

	arch->thread_fsindex = task->thread.fsindex;
	savesegment(fs, fsindex);
	if (fsindex != arch->thread_fsindex) {
		arch->thread_fsindex = fsindex;
	}

	arch->thread_gsindex = task->thread.gsindex;
	savesegment(gs, gsindex);
	if (gsindex != arch->thread_gsindex) {
		arch->thread_gsindex = gsindex;
	}

	arch->thread_fs = task->thread.fs;
	rdmsrl(MSR_FS_BASE, fs);
	if (fs != arch->thread_fs) {
		arch->thread_fs = fs;
	}

	arch->thread_gs = task->thread.gs;
	rdmsrl(MSR_KERNEL_GS_BASE, gs);
	if (gs != arch->thread_gs) {
		arch->thread_gs = gs;
	}

	PSPRINTK("%s: pc %lx sp %lx bp %lx ra %lx\n", __func__, arch->migration_pc, arch->old_rsp, arch->bp, arch->ra);

	PSPRINTK("%s: fs task %lx[%lx] saved %lx[%lx] current %lx[%lx]\n", __func__,
	      (unsigned long)task->thread.fs, (unsigned long)task->thread.fsindex,
	      (unsigned long)arch->thread_fs, (unsigned long)arch->thread_fsindex,
	      (unsigned long)fs, (unsigned long)fsindex);

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
 * 	task,	pointer to the task structure of the task of which the
 * 			architecture specific info needs to be restored
 *
 * 	arch,	pointer to the field_arch structure type from which the
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
int restore_thread_info(struct task_struct *task, field_arch *arch)
{
	int passed;
	unsigned long fsindex, fs_val;

	if ((task == NULL)  || (arch == NULL)) {
		printk(KERN_ERR"process_server: invalid params to %s", __func__);
		return -EINVAL;
	}

	/* For het migration */
	if (arch->migration_pc != 0) {
		task_pt_regs(task)->ip = arch->migration_pc;
		task_pt_regs(task)->bp = arch->bp;

		/* task_pt_regs(task)->sp = task->saved_old_rsp; */
		task_pt_regs(task)->sp = arch->old_rsp;
		task->thread.usersp = arch->old_rsp;

		/* task_pt_regs(task)->cs = __KERNEL_CS | get_kernel_rpl(); */
		//task_pt_regs(task)->cs = __USER_CS;
		//task_pt_regs(task)->ds = __USER_DS;
		//task_pt_regs(task)->es = __USER_ES;

		//printk("%s cs %lx KERNEL_CS %lx\n", __func__, task_pt_regs(task)->cs, __KERNEL_CS);

		task_pt_regs(task)->r15 = arch->regs_x86.r15;
		task_pt_regs(task)->r14 = arch->regs_x86.r14;
		task_pt_regs(task)->r13 = arch->regs_x86.r13;
		task_pt_regs(task)->r12 = arch->regs_x86.r12;
		task_pt_regs(task)->r11 = arch->regs_x86.r11;
		task_pt_regs(task)->r10 = arch->regs_x86.r10;
		task_pt_regs(task)->r9 = arch->regs_x86.r9;
		task_pt_regs(task)->r8 = arch->regs_x86.r8;
		task_pt_regs(task)->ax = arch->regs_x86.rax;
		task_pt_regs(task)->dx = arch->regs_x86.rdx;
		task_pt_regs(task)->cx = arch->regs_x86.rcx;
		task_pt_regs(task)->bx = arch->regs_x86.rbx;
		task_pt_regs(task)->si = arch->regs_x86.rsi;
		task_pt_regs(task)->di = arch->regs_x86.rdi;
	}

	task->thread.fs = arch->thread_fs;
	task->thread.fsindex = arch->thread_fsindex;

	passed = 1;
	if (current == task) {
		loadsegment(fs, arch->thread_fsindex);
		wrmsrl(MSR_FS_BASE, arch->thread_fs);
		passed = 2;
	}

	fsindex = 0x1234;
	fs_val = 0x11112222;

	savesegment(fs, fsindex);
	rdmsrl(MSR_FS_BASE, fs_val);

	PSPRINTK("%s: ip %lx sp %lx bp %lx\n", __func__, arch->migration_pc, arch->old_rsp, arch->bp);

	PSPRINTK(KERN_EMERG"%s: task=%s current=%s (%d) FS saved %lx[%lx] curr %lx[%lx]\n",
	       __func__, task->comm, current->comm, passed,
	       (unsigned long)arch->thread_fs, (unsigned long)arch->thread_fsindex,
	       (unsigned long)fs_val, (unsigned long)fsindex);

	//dump_processor_regs(task_pt_regs(task));

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
int update_thread_info(struct task_struct *task)
{
	unsigned int fsindex, gsindex;

	//printk("%s [+] TID: %d\n", __func__, task->pid);
	if(task == NULL){
		printk(KERN_ERR"%s: ERROR: process_server: invalid params\n", __func__);
		return -EINVAL;
	}

	savesegment(fs, fsindex);
	if (unlikely(fsindex | task->thread.fsindex))
		loadsegment(fs, task->thread.fsindex);
	else
		loadsegment(fs, 0);

	if (task->thread.fs)
		wrmsrl_safe(MSR_FS_BASE, task->thread.fs);

	savesegment(gs, gsindex); //read the gs register in gsindex variable
	if (unlikely(gsindex | task->thread.gsindex))
		load_gs_index(task->thread.gsindex);
	else
		load_gs_index(0);

	if (task->thread.gs)
		wrmsrl_safe(MSR_KERNEL_GS_BASE, task->thread.gs);

	//dump_processor_regs(task_pt_regs(task));
	//__show_regs(task_pt_regs(task), 1);

	//printk("%s [-] TID: %d\n", __func__, task->pid);
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
int initialize_thread_retval(struct task_struct *task, int val)
{
	//printk("%s [+] TID: %d\n", __func__, task->pid);
	if (task == NULL) {
		printk(KERN_ERR"process_server: invalid params to %s", __func__);
		return -EINVAL;
	}
	task_pt_regs(task)->ax = val;
	//printk("%s [-] TID: %d\n", __func__, task->pid);

	return 0;
}

/*
 * Function:
 *		create_thread
 *
 * Description:
 *		this function creates an empty thread and returns its task
 *		structure
 *
 * Input:
 * 	flags,	the clone flags to be used to create the new thread
 *
 * Output:
 * 	none
 *
 * Return value:
 * 	on success,	returns pointer to newly created task's structure,
 *	on failure, returns NULL
 */
struct task_struct* create_thread(int flags)
{
	struct task_struct *task = NULL;
	struct pt_regs regs;

	memset(&regs, 0, sizeof(struct pt_regs));

	current->flags &= ~PF_KTHREAD;
	task = do_fork_for_main_kernel_thread(flags, 0, &regs, 0, NULL, NULL);
	current->flags |= PF_KTHREAD;

	if (task != NULL) {
		PSPRINTK("%s [-]: task = %p\n", __func__, task);
	} else {
		printk("%s [-]: do_fork failed, task = %p at %p\n", __func__, task, &task);
	}

	return task;
}

#if MIGRATE_FPU

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
int save_fpu_info(struct task_struct *task, field_arch *arch)
{
	struct fpu temp;

	//printk("%s [+] TID: %d\n", __func__, task->pid);
	if ((task == NULL)  || (arch == NULL)) {
		printk(KERN_ERR"process_server: invalid params to %s", __func__);
		return -EINVAL;
	}

	//FPU migration code --- initiator
	//PSPRINTK(KERN_ERR "%s: task flags %x fpu_counter %x has_fpu %x [%d:%d] %d:%d %x\n",
	//		__func__, task->flags, (int)task->fpu_counter, (int)task->thread.has_fpu,
	//		(int)__thread_has_fpu(task), (int)fpu_allocated(&task->thread.fpu),
	//		(int)use_xsave(), (int)use_fxsr(), (int) PF_USED_MATH);

	arch->task_flags = task->flags;
	arch->task_fpu_counter = task->thread.fpu.counter;
	arch->thread_has_fpu = task->thread.fpu.fpregs_active;

	//    if (__thread_has_fpu(task)) {
	if (!fpu_allocated(&task->thread.fpu)){
		fpu_alloc(&task->thread.fpu);
		fpu_finit(&task->thread.fpu);
	}

	fpu_save_init(&task->thread.fpu);

	temp.state = &request->fpu_state;
	fpu_copy(&temp,&task->thread.fpu);
	//printk("%s [-] TID: %d\n", __func__, task->pid);

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
int restore_fpu_info(struct task_struct *task, field_arch *arch)
{
	//printk("%s [+] TID: %d\n", __func__, task->pid);
	if((task == NULL)  || (arch == NULL)){
		printk(KERN_ERR"process_server: invalid params to %s", __func__);
		return -EINVAL;
	}

	//FPU migration code --- server
	/* PF_USED_MATH is set if the task used the FPU before
	 * fpu_counter is incremented every time you go in __switch_to while owning the FPU
	 * has_fpu is true if the task is the owner of the FPU, thus the FPU contains its data
	 * fpu.preload (see arch/x86/include/asm.i387.h:switch_fpu_prepare()) is a heuristic
	 */
	if (arch->task_flags & PF_USED_MATH)
	//set_used_math();
	set_stopped_child_used_math(task);

	task->fpu_counter = arch->task_fpu_counter;

	if (!fpu_allocated(&task->thread.fpu)) {
		fpu_alloc(&task->thread.fpu);
		fpu_finit(&task->thread.fpu);
	}

	struct fpu temp; temp.state = &arch->fpu_state;
	fpu_copy(&task->thread.fpu, &temp);

	//PSPRINTK(KERN_ERR "%s: task flags %x fpu_counter %x has_fpu %x [%d:%d]\n",
	//		__func__, task->flags, (int)task->fpu_counter, (int)task->thread.has_fpu,
	//		(int)__thread_has_fpu(task), (int)fpu_allocated(&task->thread.fpu));

	//FPU migration code --- is the following optional?
	if (tsk_used_math(task) && task->fpu_counter >5)//fpu.preload
	__math_state_restore(task);

	//printk("%s [-] TID: %d\n", __func__, task->pid);
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
int update_fpu_info(struct task_struct *task)
{
	//printk("%s [+] TID: %d\n", __func__, task->pid);
	if (task == NULL){
		printk(KERN_ERR"process_server: invalid params to %s", __func__);
		return -EINVAL;
	}

	if (tsk_used_math(task) && task->fpu_counter >5) //fpu.preload
		__math_state_restore(task);
	//printk("%s [-] TID: %d\n", __func__, task->pid);

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
 *	on success, returns 0
 * 	on failure, returns negative integer
 */
int dump_processor_regs(struct pt_regs* regs)
{
	unsigned long fs, gs;
	unsigned long fsindex, gsindex;

	if (regs == NULL) {
		printk(KERN_ERR"process_server: invalid params to %s", __func__);
		return -EINVAL;
	}
	dump_stack();
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

	return 0;
}

#if 0 // beowulf. removed
void suggest_migration(int suggestion)
{
	vpopcorn_migrate = suggestion;
}
#endif
