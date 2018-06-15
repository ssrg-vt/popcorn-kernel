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

struct remote_context;

void process_vma_info_request(vma_info_request_t *req);

void process_vma_op_request(vma_op_request_t *req);

#endif /* KERNEL_POPCORN_VMA_SERVER_H_ */
