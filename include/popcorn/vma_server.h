// SPDX-License-Identifier: GPL-2.0, 3-clause BSD
#ifndef INCLUDE_POPCORN_VMA_SERVER_H_
#define INCLUDE_POPCORN_VMA_SERVER_H_


/*
 * VMA operation handlers for origin
 */
int vma_server_munmap_origin(unsigned long start, size_t len, int nid_except);


/*
 * Retrieve VMAs from origin
 */
int vma_server_fetch_vma(struct task_struct *tsk, unsigned long address);

/*
 * VMA operation handler for remote
 */
unsigned long vma_server_mmap_remote(struct file *file, unsigned long addr,
				     unsigned long len, unsigned long prot,
				     unsigned long flags, unsigned long pgoff);

int vma_server_munmap_remote(unsigned long start, size_t len);
int vma_server_brk_remote(unsigned long oldbrk, unsigned long brk);
int vma_server_madvise_remote(unsigned long start, size_t len, int behavior);
int vma_server_mprotect_remote(unsigned long start, size_t len,
			       unsigned long prot);
int vma_server_mremap_remote(unsigned long addr, unsigned long old_len,
			     unsigned long new_len, unsigned long flags,
			     unsigned long new_addr);

#endif /* INCLUDE_POPCORN_VMA_SERVER_H_ */
