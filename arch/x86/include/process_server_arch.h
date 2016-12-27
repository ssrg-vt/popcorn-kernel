/*
 * File:
 * 	process_server_arch.h
 *
 * Description:
 * 	this file provides the headers of the architecture specific
 *  helper functionality implementation of the process server
 *
 * Created on:
 * 	Sep 19, 2014
 *
 * Author:
 * 	Sharath Kumar Bhat, SSRG, VirginiaTech
 *
 */

#ifndef PROCESS_SERVER_ARCH_H_
#define PROCESS_SERVER_ARCH_H_

/*
 * Functions declarations
 */
extern int save_thread_info(struct task_struct *task, struct pt_regs *regs, field_arch *arch, void __user *uregs);
extern int restore_thread_info(struct task_struct *task, field_arch *arch);
extern int update_thread_info(struct task_struct *task);
extern int initialize_thread_retval(struct task_struct *task, int val);
extern struct task_struct* create_thread(int flags);
extern int dump_processor_regs(struct pt_regs* regs);
extern void suggest_migration(int suggestion);

/* FPU related functions */
#ifdef MIGRATE_FPU

extern int save_fpu_info(struct task_struct *task, field_arch *arch);
extern int restore_fpu_info(struct task_struct *task, field_arch *arch);
extern int update_fpu_info(struct task_struct *task);

#endif

#endif /* PROCESS_SERVER_ARCH_H_ */
