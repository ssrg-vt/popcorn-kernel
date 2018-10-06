#ifndef __KERNEL_POPCORN_PROCESS_SERVER_H__
#define __KERNEL_POPCORN_PROCESS_SERVER_H__

struct task_struct;
struct field_arch;

int save_thread_info(struct field_arch *arch);
int restore_thread_info(struct field_arch *arch, bool restore_segments);
#endif
