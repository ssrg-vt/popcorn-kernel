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

void vma_worker_remote(struct remote_context *rc);

void process_remote_vma_request(struct pcn_kmsg_message *msg);
void process_remote_vma_op(vma_op_request_t *req);

#endif /* KERNEL_POPCORN_VMA_SERVER_H_ */
