#ifndef __POPCORN_PROCESS_SERVER_H
#define __POPCORN_PROCESS_SERVER_H

int process_server_do_migration(struct task_struct* tsk, int cpu,
                                struct pt_regs* regs, void __user *uregs);
int process_server_task_exit_notification(struct task_struct *tsk,long code);

int popcorn_process_exit(struct task_struct *tsk);

#endif /* __POPCORN_PROCESS_SERVER_H */
