/*
 * File:
 *	process_server.c
 *
 * Description:
 *	Helper functionality of the process server
 *
 * @author Cesar Philippidis, RASEC Technologies 2020
 */

/* File includes */
#include <linux/sched.h>
#include <linux/uaccess.h>
#include <linux/ptrace.h>
#include <linux/sched/task_stack.h>

#include <asm/compat.h>
#include <asm/switch_to.h>

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
 *	on failure, returns negative integer
 */
int save_thread_info(struct field_arch *arch)
{
	int cpu;

	cpu = get_cpu();

	asm("mv %0, tp;" : "=r"(arch->tls));

	put_cpu();

#ifdef CONFIG_POPCORN_DEBUG_VERBOSE
	PSPRINTK("%s [%d] tls %lx\n", __func__, current->pid, arch->tls);
#endif

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
 *	arch,	pointer to the struct field_arch structure type from which the
 *			architecture specific information of the task has to be
 *			restored
 *
 * Output:
 *	none
 *
 * Return value:
 *	on success, returns 0
 *	on failure, returns negative integer
 */
int restore_thread_info(struct field_arch *arch, bool restore_segments)
{
	struct pt_regs *regs = current_pt_regs();
	struct regset_riscv64 *regset = &arch->regs_riscv;
	int cpu;

	cpu = get_cpu();

	regs->sp = regset->sp;
	regs->sepc = regset->pc;

	regs->ra = regset->x[1];
	//regs->sp = regset->x[2];
	regs->gp = regset->x[3];
	//regs->tp = regset->x[4];
	regs->t0 = regset->x[5];
	regs->t1 = regset->x[6];
	regs->t2 = regset->x[7];
	regs->s0 = regset->x[8];
	regs->s1 = regset->x[9];
	regs->a0 = regset->x[10];
	regs->a1 = regset->x[11];
	regs->a2 = regset->x[12];
	regs->a3 = regset->x[13];
	regs->a4 = regset->x[14];
	regs->a5 = regset->x[15];
	regs->a6 = regset->x[16];
	regs->a7 = regset->x[17];
	regs->s2 = regset->x[18];
	regs->s3 = regset->x[19];
	regs->s4 = regset->x[20];
	regs->s5 = regset->x[21];
	regs->s6 = regset->x[22];
	regs->s7 = regset->x[23];
	regs->s8 = regset->x[24];
	regs->s9 = regset->x[25];
	regs->s10 = regset->x[26];
	regs->s11 = regset->x[27];
	regs->t3 = regset->x[28];
	regs->t4 = regset->x[29];
	regs->t5 = regset->x[30];
	regs->t6 = regset->x[31];

	if (restore_segments) {
		regs->tp = arch->tls;
		regs->sstatus = 0;
		regs->sstatus |= SR_FS_INITIAL;
		fstate_restore(current, regs);
	}

	put_cpu();

	return 0;
}


noinline_for_stack void update_frame_pointer(void)
{
	unsigned long *rfp;
	rfp = __builtin_frame_address(0);

	/* User rbp is at one stack frames below */
	*rfp = current_pt_regs()->s0;	/* sched_migrate */
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
 *	task,	pointer to the architecture specific registers
 *
 * Output:
 *	none
 *
 * Return value:
 *	on success, returns 0
 *	on failure, returns negative integer
 */
int dump_processor_regs(struct pt_regs *regs)
{
	dump_stack();
	if (!regs)
		return 0;

	printk(KERN_ALERT"DUMP REGS %s\n", __func__);

	if (NULL != regs) {
		printk(KERN_ALERT"sepc{%lx}\n",regs->sepc);
		printk(KERN_ALERT"ra{%lx}\n",regs->ra);
		printk(KERN_ALERT"sp{%lx}\n",regs->sp);
		printk(KERN_ALERT"gp{%lx}\n",regs->gp);
		printk(KERN_ALERT"tp{%lx}\n",regs->tp);
		printk(KERN_ALERT"t0{%lx}\n",regs->t0);
		printk(KERN_ALERT"t1{%lx}\n",regs->t1);
		printk(KERN_ALERT"t2{%lx}\n",regs->t2);
		printk(KERN_ALERT"fp{%lx}\n",regs->s0);
		printk(KERN_ALERT"s1{%lx}\n",regs->s1);
		printk(KERN_ALERT"a0{%lx}\n",regs->a0);
		printk(KERN_ALERT"a1{%lx}\n",regs->a1);
		printk(KERN_ALERT"a2{%lx}\n",regs->a2);
		printk(KERN_ALERT"a3{%lx}\n",regs->a3);
		printk(KERN_ALERT"a4{%lx}\n",regs->a4);
		printk(KERN_ALERT"a5{%lx}\n",regs->a5);
		printk(KERN_ALERT"a6{%lx}\n",regs->a6);
		printk(KERN_ALERT"a7{%lx}\n",regs->a7);
		printk(KERN_ALERT"s2{%lx}\n",regs->s2);
		printk(KERN_ALERT"s3{%lx}\n",regs->s3);
		printk(KERN_ALERT"s4{%lx}\n",regs->s4);
		printk(KERN_ALERT"s5{%lx}\n",regs->s5);
		printk(KERN_ALERT"s6{%lx}\n",regs->s6);
		printk(KERN_ALERT"s7{%lx}\n",regs->s7);
		printk(KERN_ALERT"s8{%lx}\n",regs->s8);
		printk(KERN_ALERT"s9{%lx}\n",regs->s9);
		printk(KERN_ALERT"s10{%lx}\n",regs->s10);
		printk(KERN_ALERT"s11{%lx}\n",regs->s11);
		printk(KERN_ALERT"t3{%lx}\n",regs->t3);
		printk(KERN_ALERT"t4{%lx}\n",regs->t4);
		printk(KERN_ALERT"t5{%lx}\n",regs->t5);
		printk(KERN_ALERT"t6{%lx}\n",regs->t6);
		printk(KERN_ALERT"sstatus{%lx}\n",regs->sstatus);
		printk(KERN_ALERT"sbadaddr{%lx}\n",regs->sbadaddr);
		printk(KERN_ALERT"scause{%lx}\n",regs->scause);
		printk(KERN_ALERT"orig_a0{%lx}\n",regs->orig_a0);
	}

	return 0;
}
