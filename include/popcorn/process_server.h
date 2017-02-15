#ifndef __POPCORN_PROCESS_SERVER_H
#define __POPCORN_PROCESS_SERVER_H

#include <popcorn/types.h>

extern int save_thread_info(struct task_struct *task,
		struct pt_regs *regs, field_arch *arch, void __user *uregs);
extern int restore_thread_info(struct task_struct *task, field_arch *arch);
extern int update_thread_info(void);
extern int initialize_thread_retval(struct task_struct *task, int val);
extern void dump_processor_regs(struct pt_regs* regs);


/* FPU related functions */
#ifdef MIGRATE_FPU
extern int save_fpu_info(struct task_struct *task, field_arch *arch);
extern int restore_fpu_info(struct task_struct *task, field_arch *arch);
extern int update_fpu_info(struct task_struct *task);
#endif


int process_server_do_migration(struct task_struct* tsk, int cpu, void __user *uregs);
int process_server_task_exit_notification(struct task_struct *tsk,long code);

int process_server_task_exit(struct task_struct *tsk);

static inline bool process_is_distributed(struct task_struct *tsk)
{
	if (!tsk->mm) return false;
	return !!tsk->mm->remote;
}
void exit_remote_context(struct remote_context *);
#endif /* __POPCORN_PROCESS_SERVER_H */
