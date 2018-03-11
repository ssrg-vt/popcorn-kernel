/*
 * Popcorn page prefecting interface
 */

#ifndef PREFETCH_H
#define PREFETCH_H

#define MAX_PF_REQ 100 /* max # of prefetching addr requests */

struct prefetch_body {
    unsigned long addr;
    int prio;
	bool populated;
} __attribute__((packed));

struct prefetch_list {
    struct prefetch_body pf_objs[MAX_PF_REQ];
} __attribute__((packed));

struct prefetch_list *alloc_prefetch_list(void);
void free_prefetch_list(struct prefetch_list* pf_list);
void prefetch_policy(struct prefetch_list* pf_list, unsigned long fault_addr);
struct prefetch_list *select_prefetch_pages(
					struct prefetch_list* pf_list, struct mm_struct *mm);
#endif /* !PREFETCH_H */
