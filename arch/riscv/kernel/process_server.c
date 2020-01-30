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
	return 0;
}


noinline_for_stack void update_frame_pointer(void)
{
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
	return 0;
}

unsigned long futex_atomic_add(unsigned long ptr, unsigned long val)
{
	return 0;
}
