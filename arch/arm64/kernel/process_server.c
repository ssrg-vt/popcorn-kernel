/*
 * File:
 * 	process_server.c
 *
 * Description:
 * 	Dummy file to support helper functionality 
 * of the process server
 *
 * Created on:
 * 	Sep 19, 2014
 *
 * Author:
 * 	Ajithchandra Saya, SSRG, VirginiaTech
 *
 */

/* File includes */
#include <linux/sched.h>
#include <linux/uaccess.h>
#include <linux/cpu_namespace.h>
#include <linux/ptrace.h>

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
int save_thread_info(struct task_struct *task, field_arch *arch, void __user *uregs)
{
	struct pt_regs *regs = task_pt_regs(task);
	int cpu;

	//dump_processor_regs(task_pt_regs(task));

	cpu = get_cpu();

	memcpy(&arch->regs, regs, sizeof(*regs));

	if (uregs) {
		/*
		int ret = copy_from_user(&arch->regs_x86, uregs,
				sizeof(struct popcorn_regset_x86_64));
		BUG_ON(ret != 0);
		*/

		arch->migration_ip = task->migration_ip;
		arch->ip = regs->user_regs.pc;
		arch->bp = regs->user_regs.regs[29];
		arch->sp = regs->user_regs.sp;
	} else {
		arch->migration_ip = regs->user_regs.pc;
		arch->ip = regs->user_regs.pc;
		arch->bp = regs->user_regs.regs[29];
		arch->sp = regs->user_regs.sp;
	}

	arch->thread_fs = task->thread.tp_value;

	put_cpu();

	PSPRINTK("%s: pc %lx sp %lx bp %lx tp %lx\n", __func__,
			arch->migration_ip, arch->sp, arch->bp, arch->thread_fs);
	show_regs(regs);

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
int restore_thread_info(struct task_struct *task, field_arch *arch, bool restore_segments)
{
	struct pt_regs *regs = task_pt_regs(task);
	int cpu;

	cpu = get_cpu();
	memcpy(regs, &arch->regs, sizeof(*regs));

	regs->user_regs.pc = arch->migration_ip;
	regs->user_regs.pstate = PSR_MODE_EL0t;
	//regs->user_regs.sp = arch->regs.sp;
	//regs->regs[29] = arch->bp;
	//regs->regs[30] = arch->ra;

	if (restore_segments) {
		task->thread.tp_value = arch->thread_fs;
	}

	/*
	for (i = 0; i < 31; i++)
		regs->regs[i] =  arch->regs_aarch.x[i];
	

	if(arch->migration_ip != 0)
		task_pt_regs(task)->user_regs.pc = arch->migration_ip;
	*/
	put_cpu();

	PSPRINTK("%s: pc %lx sp %lx bp %lx\n", __func__,
			arch->migration_ip, arch->sp, arch->bp);
	show_regs(regs);

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
	printk("%s\n", __func__);

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
	return 0;
}

#ifdef MIGRATE_FPU

/*
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
	return 0;
}

#endif

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
int dump_processor_regs(struct pt_regs *regs)
{
	int i;

	if (regs == NULL) {
		printk(KERN_ERR"process_server: invalid params to dump_processor_regs()");
		return 0;
	}

	dump_stack();

	printk("DUMP REGS %s\n", __func__);

	if (NULL != regs) {
		printk("sp: 0x%llx\n", regs->sp);
		printk("pc: 0x%llx\n", regs->pc);
		printk("pstate: 0x%llx\n", regs->pstate);

		for (i = 0; i < 31; i++) {
			printk("regs[%d]: 0x%llx\n", i, regs->regs[i]);
		}
	}

	return 0;
}

unsigned long futex_atomic_add(unsigned long ptr, unsigned long val)
{
	atomic64_t v;
	unsigned long result;
	v.counter = ptr;
	
	result = atomic64_add_return(val, &v);
	return (result - val);
}
