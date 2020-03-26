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

#ifndef __INTERNAL_VMA_SERVER_H_
#define __INTERNAL_VMA_SERVER_H_

#include <popcorn/vma_server.h>

enum vma_op_code {
	VMA_OP_NOP = -1,
	VMA_OP_MMAP,
	VMA_OP_MUNMAP,
	VMA_OP_MPROTECT,
	VMA_OP_MREMAP,
	VMA_OP_MADVISE,
	VMA_OP_BRK,
	VMA_OP_MAX,
};

struct remote_context;

void process_vma_info_request(vma_info_request_t *req);

void process_vma_op_request(vma_op_request_t *req);

#endif /* __INTERNAL_VMA_SERVER_H_ */
