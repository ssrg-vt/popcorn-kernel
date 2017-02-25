/**
 * @file vma_server.h
 * (private interface)
 *
 * Popcorn Linux VMA server private interface
 * This work is an extension of David Katz MS Thesis, please refer to the
 * Thesis for further information about the algorithm.
 *
 * @author Antonio Barbalace, SSRG Virginia Tech 2016
 */

#ifndef KERNEL_POPCORN_VMA_SERVER_H_
#define KERNEL_POPCORN_VMA_SERVER_H_

#include "types.h"

/**
 * Creates a local mapping for the VMA -- this is the case in which is the local
 * kernel that decides how to create the mapping.
 *
 * @return Returns 0 on success, an error code otherwise.
 */
struct mapping_answers_for_2_kernels;

int vma_server_do_mapping_for_distributed_process(
		struct mapping_answers_for_2_kernels* fetching_page,
		struct task_struct *tsk, struct mm_struct* mm,
		unsigned long address, spinlock_t* ptl);

/**
 * This is used to locally enqueue work to the vma_server (specifically
 * vma_server_process_vma_op server). The usual way to enqueue work is by
 * messages, but this is a shortcut to do not overload the messaging layer.
 *
 * @return Return 0 on success, an error code otherwise.
 */
int vma_server_enqueue_vma_op(memory_t * memory, vma_operation_t * operation,
		int fake);

void vma_worker_main(struct remote_context *rc, const char *at);

#endif /* KERNEL_POPCORN_VMA_SERVER_H_ */
