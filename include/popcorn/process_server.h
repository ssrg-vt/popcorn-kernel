
#ifndef __POPCORN_PROCESS_SERVER_H
#define __POPCORN_PROCESS_SERVER_H

#include <process_server_arch_macros.h>
#include <process_server_arch.h>
#include <popcorn/process_server_macro.h>

/**
 * Use the preprocessor to turn off printk.
 */
#define MIGRATION_PROFILE		1
#define PROCESS_SERVER_VERBOSE 		0
#define PROCESS_SERVER_VMA_VERBOSE 	0
#define PROCESS_SERVER_NEW_THREAD_VERBOSE 0
#define PROCESS_SERVER_MINIMAL_PGF_VERBOSE 0

#define MIGRATE_FPU 0

#define PROCESS_SERVER_CLONE_SUCCESS 0
#define PROCESS_SERVER_CLONE_FAIL 1

//#define MAX_KERNEL_IDS NR_CPUS
#define MAX_KERNEL_IDS 2

#define VMA_OP_NOP 0
#define VMA_OP_UNMAP 1
#define VMA_OP_PROTECT 2
#define VMA_OP_REMAP 3
#define VMA_OP_MAP 4
#define VMA_OP_BRK 5
#define VMA_OP_MADVISE 6

#define VMA_OP_SAVE -70
#define VMA_OP_NOT_SAVE -71

#define EXIT_ALIVE 0
#define EXIT_THREAD 1
#define EXIT_PROCESS 2
#define EXIT_FLUSHING 3
#define EXIT_NOT_ACTIVE 4

#define REPLICATION_STATUS_VALID 3
#define REPLICATION_STATUS_WRITTEN 1
#define REPLICATION_STATUS_INVALID 2
#define REPLICATION_STATUS_NOT_REPLICATED 0

int process_server_do_migration(struct task_struct* task, int cpu,
                                struct pt_regs* regs, void __user *uregs);
int process_server_dup_task(struct task_struct* orig, struct task_struct* task);
void page_server_clean_page(struct page* page);
int page_server_update_page(struct task_struct * tsk, struct mm_struct *mm,
                               struct vm_area_struct *vma,
                               unsigned long address,
                               unsigned long page_fault_flags, int retrying);
int page_server_try_handle_mm_fault(struct task_struct *tsk,
                                       struct mm_struct *mm,
                                       struct vm_area_struct *vma,
		                       unsigned long address,
                                       unsigned long page_fault_flags,
                                       unsigned long error_code);
int process_server_task_exit_notification(struct task_struct *tsk, long code);
void sleep_shadow(void);
void create_thread_pull(void);

void synchronize_migrations(int tgroup_home_cpu,int tgroup_home_id);

#endif /* __POPCORN_PROCESS_SERVER_H */
