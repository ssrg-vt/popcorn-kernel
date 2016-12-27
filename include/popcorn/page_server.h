/**
 * @file page_server.h
 * (public interface)
 *
 * Popcorn Linux page server public interface
 * This work is an extension of Marina Sadini MS Thesis, plese refer to the
 * Thesis for further information about the algorithm.
 *
 * @author Antonio Barbalace, SSRG Virginia Tech 2016
 */

#ifndef INCLUDE_POPCORN_PAGE_SERVER_H_
#define INCLUDE_POPCORN_PAGE_SERVER_H_

/**
 * Updates the information of a memory page (usually from the page handler).
 * The information should be stored in the find_mapping_entry list.
 *
 * @return Returns 0 on success, an error code otherwise.
 */
int page_server_update_page(struct task_struct * tsk, struct mm_struct *mm,
			       struct vm_area_struct *vma, unsigned long address_not_page, unsigned long page_fault_flags,
				   int retrying);

/**
 * Cleans Popcorn Linux's related fields when a memory page is returned to the
 * system.
 *
 * @return This function always succeeds, if page is NULL it just returns.
 */
void page_server_clean_page(struct page* page);

/**
 * Main function of the page server usually called when there is a page fault.
 * This function maintains the consistency of each page in the page consistency protocol,
 * for example this function can be called on a CPU before writing to a specific page that
 * is working on the page consistency protocol. It behaves like try_handle_mm_fault().
 *
 * @return Returns 0 on success, an error code otherwise.
 */
int page_server_try_handle_mm_fault(struct task_struct *tsk,
				  struct mm_struct *mm, struct vm_area_struct *vma,
				  unsigned long page_fault_address, unsigned long page_fault_flags,
				  unsigned long error_code);

#endif /* INCLUDE_POPCORN_PAGE_SERVER_H_ */
