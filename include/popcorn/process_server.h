
#ifndef __POPCORN_PROCESS_SERVER_H
#define __POPCORN_PROCESS_SERVER_H

#include <process_server_arch_macros.h>
#include <process_server_arch.h>
#include <popcorn/process_server_macro.h>

/**
 * Use the preprocessor to turn off printk.
 */
int process_server_do_migration(struct task_struct* task, int cpu,
                                struct pt_regs* regs, void __user *uregs);
int process_server_dup_task(struct task_struct* orig, struct task_struct* task);
void page_server_clean_page(struct page* page);
int page_server_update_page(struct task_struct * tsk, struct mm_struct *mm,
                               struct vm_area_struct *vma,
                               unsigned long address,
                               unsigned long page_fault_flags, int retrying);
int page_server_try_handle_mm_fault(
		struct task_struct *tsk,
		struct mm_struct *mm,
		struct vm_area_struct *vma,
		unsigned long address,
		unsigned long page_fault_flags,
		unsigned long error_code);
int process_server_task_exit_notification(struct task_struct *tsk, long code);
void sleep_shadow(void);

void synchronize_migrations(int tgroup_home_cpu,int tgroup_home_id);

#endif /* __POPCORN_PROCESS_SERVER_H */
