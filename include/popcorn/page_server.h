/**
 * @file page_server.h
 * (public interface)
 *
 * Popcorn Linux page server public interface
 * This work is an extension of Marina Sadini MS Thesis, plese refer to the
 * Thesis for further information about the algorithm.
 *
 * @author Sang-Hoon Kim, SSRG Virginia Tech 2017
 * @author Antonio Barbalace, SSRG Virginia Tech 2016
 */

#ifndef INCLUDE_POPCORN_PAGE_SERVER_H_
#define INCLUDE_POPCORN_PAGE_SERVER_H_

struct fault_handle;

/*
 * Entry points for dealing with page fault in Popcorn Rack
 */
int page_server_handle_pte_fault(struct vm_fault *vmf);

/*
 * Flush pages in remote to the origin
 */
//int page_server_flush_remote_pages(void);

void page_server_zap_pte(
	struct vm_area_struct *vma, unsigned long addr, pte_t *pte, pte_t *pteval);

int page_server_get_userpage(u32 __user *uaddr, struct fault_handle **handle, char *mode);
void page_server_put_userpage(struct fault_handle *fh, char *mode);

void page_server_start_mm_fault(unsigned long address);
int page_server_end_mm_fault(int ret);

void page_server_panic(bool condition, struct mm_struct *mm, unsigned long address, pte_t *pte, pte_t pte_val);

int page_server_release_page_ownership(struct vm_area_struct *vma, unsigned long addr);

/* fDSM Functions, added for fpga changes */

void update_pkey(unsigned long pkey, unsigned long addr);
void delete_pkeys(void);

#endif /* INCLUDE_POPCORN_PAGE_SERVER_H_ */
