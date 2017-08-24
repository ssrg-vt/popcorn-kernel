#ifndef __POPCORN_PROCESS_SERVER_H
#define __POPCORN_PROCESS_SERVER_H

struct task_struct;

int process_server_do_migration(struct task_struct* tsk, unsigned int dst_nid, void __user *uregs);
int process_server_task_exit(struct task_struct *tsk);
int update_frame_pointer(void);

#endif /* __POPCORN_PROCESS_SERVER_H */
