#ifndef __POPCORN_PROCESS_SERVER_H
#define __POPCORN_PROCESS_SERVER_H

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
