/**
 * @file vma_server.h
 * (public interface)
 *
 * Popcorn Linux VMA server public interface
 * This work is an extension of David Katz MS Thesis, please refer to the
 * Thesis for further information about the algorithm.
 *
 * @author Antonio Barbalace, SSRG Virginia Tech 2016
 */

#ifndef INCLUDE_POPCORN_VMA_SERVER_H_
#define INCLUDE_POPCORN_VMA_SERVER_H_

#include <popcorn/types.h>

/**
 * This function takes a distributed lock for a VMA operation, and triggers
 * the same operation among different Popcorn Linux kernels.
 *
 * @return Returns either a valid memory address for the distributed operation
 *         or an error code in case of failures.
 */
long start_distribute_operation(int operation, unsigned long addr, size_t len,
		unsigned long prot, unsigned long new_addr, unsigned long new_len,
		unsigned long flags, struct file *file, unsigned long pgoff);
/**
 * This function coordinates the end of a distributed VMA operation among
 * different Popcorn Linux kernels.
 *
 * @return The function doesn't return an error but it can either print an error
 *         message on the kernel log or throw a kernel bug.
 */
void end_distribute_operation(int operation, long start_ret, unsigned long addr);

/**
 * NOTE
 * The followings are wrappers around the start/end_distribute_operation
 * functions. start/end_distribute_operation create a distributed lock among
 * Popcorn Linux kernels to carry on VMA operations. David Katz's MS Thesis
 * details the functioning. For any additional VMA operation that has to be
 * carried in a distributed fashion a new function must be added in the
 * following and the relative handling code should also be added in
 * start_distribute_operation and end_distribute_operation.
 */
static inline long vma_server_madvise_remove_start(struct mm_struct *mm,
		unsigned long start, size_t len)
{
	return start_distribute_operation(VMA_OP_MADVISE, start, len, 0, 0, 0, 0,
			NULL, 0);
}
static inline long vma_server_madvise_remove_end(struct mm_struct *mm,
		unsigned long start, size_t len, int start_ret)
{
	end_distribute_operation(VMA_OP_MADVISE, start_ret, start);
	return 0;
}
static inline long vma_server_do_unmap_start(struct mm_struct *mm,
		unsigned long start, size_t len)
{
	return start_distribute_operation(VMA_OP_UNMAP, start, len, 0, 0, 0, 0,
			NULL, 0);
}
static inline long vma_server_do_unmap_end(struct mm_struct *mm,
		unsigned long start, size_t len, int start_ret)
{
	end_distribute_operation(VMA_OP_UNMAP, start_ret, start);
	return 0;
}
static inline long vma_server_mprotect_start(unsigned long start, size_t len,
		unsigned long prot)
{
	return start_distribute_operation(VMA_OP_PROTECT, start, len, prot, 0, 0, 0,
			NULL, 0);
}
static inline long vma_server_mprotect_end(unsigned long start, size_t len,
		unsigned long prot, int start_ret)
{
	end_distribute_operation(VMA_OP_PROTECT, start_ret, start);
	return 0;
}
static inline long vma_server_do_mmap_pgoff_start(struct file *file,
		unsigned long addr, unsigned long len, unsigned long prot,
		unsigned long flags, unsigned long pgoff)
{
	return start_distribute_operation(VMA_OP_MAP, addr, len, prot, 0, 0, flags,
			file, pgoff);
}
static inline long vma_server_do_mmap_pgoff_end(struct file *file,
		unsigned long addr, unsigned long len, unsigned long prot,
		unsigned long flags, unsigned long pgoff, unsigned long start_ret)
{
	end_distribute_operation(VMA_OP_MAP, start_ret, addr);
	return 0;
}
static inline long vma_server_do_brk_start(unsigned long addr,
		unsigned long len)
{
	return start_distribute_operation(VMA_OP_BRK, addr, len, 0, 0, 0, 0, NULL,
			0);
}
static inline long vma_server_do_brk_end(unsigned long addr, unsigned long len,
		unsigned long start_ret)
{
	end_distribute_operation(VMA_OP_BRK, start_ret, addr);
	return 0;
}
static inline long vma_server_do_mremap_start(unsigned long addr,
		unsigned long old_len, unsigned long new_len, unsigned long flags,
		unsigned long new_addr)
{
	return start_distribute_operation(VMA_OP_REMAP, addr, (size_t) old_len, 0,
			new_addr, new_len, flags, NULL, 0);
}
static inline long vma_server_do_mremap_end(unsigned long addr,
		unsigned long old_len, unsigned long new_len, unsigned long flags,
		unsigned long new_addr, unsigned long start_ret)
{
	end_distribute_operation(VMA_OP_REMAP, start_ret, new_addr);
	return 0;
}

int vma_server_fetch_vma(struct task_struct *tsk, unsigned long address);

#endif /* INCLUDE_POPCORN_VMA_SERVER_H_ */
