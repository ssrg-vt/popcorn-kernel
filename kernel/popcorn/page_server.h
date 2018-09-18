#ifndef __KERNEL_POPCORN_PAGE_SERVER_H__
#define __KERNEL_POPCORN_PAGE_SERVER_H__

int page_server_flush_remote_pages(struct remote_context *rc);

/* Implemented in mm/memory.c */
int handle_pte_fault_origin(struct mm_struct *, struct vm_area_struct *, unsigned long, pte_t *, pmd_t *, unsigned int);
struct page *get_normal_page(struct vm_area_struct *vma, unsigned long addr, pte_t *pte);
int cow_file_at_origin(struct mm_struct *mm, struct vm_area_struct *vma, unsigned long addr, pte_t *pte);

void free_remote_context_pages(struct remote_context *rc);
int process_madvise_release_from_remote(int from_nid, unsigned long start, unsigned long end);


struct page *__get_page_info_page(struct mm_struct *mm, unsigned long addr, unsigned long *offset);

void __revoke_page_ownership(struct task_struct *tsk, int nid, pid_t pid, unsigned long addr, int ws_id);

struct remote_context *get_task_remote(struct task_struct *tsk);

bool put_task_remote(struct task_struct *tsk);

void __revoke_page_ownerships(struct task_struct *tsk, int nid, pid_t pid, unsigned long *addr, unsigned long tso_wr_cnt);
#endif
