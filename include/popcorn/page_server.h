// SPDX-License-Identifier: GPL-2.0, 3-clause BSD
#ifndef INCLUDE_POPCORN_PAGE_SERVER_H_
#define INCLUDE_POPCORN_PAGE_SERVER_H_

struct fault_handle;

/*
 * Entry points for dealing with page fault in Popcorn Rack
 */
int page_server_handle_pte_fault(struct vm_fault *vmf);

void page_server_zap_pte(struct vm_area_struct *vma, unsigned long addr,
			 pte_t *pte, pte_t *pteval);

int page_server_get_userpage(u32 __user *uaddr, struct fault_handle **handle,
			     char *mode);
void page_server_put_userpage(struct fault_handle *fh, char *mode);

void page_server_panic(bool condition, struct mm_struct *mm,
		       unsigned long address, pte_t *pte,
		       pte_t pte_val);

int page_server_release_page_ownership(struct vm_area_struct *vma,
				       unsigned long addr);

/* Implemented in mm/memory.c */
int handle_pte_fault_origin(struct mm_struct *, struct vm_area_struct *,
			    unsigned long, pte_t *, pmd_t *, unsigned int);
struct page *get_normal_page(struct vm_area_struct *vma, unsigned long addr,
			     pte_t *pte);
int cow_file_at_origin(struct mm_struct *mm, struct vm_area_struct *vma,
		       unsigned long addr, pte_t *pte);

#endif /* INCLUDE_POPCORN_PAGE_SERVER_H_ */
