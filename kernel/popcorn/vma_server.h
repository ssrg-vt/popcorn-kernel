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

/* Legacy definitions. Remove these quickly -----------------------*/
#define VMA_OPERATION_FIELDS \
	int origin_nid; \
	int origin_pid; \
	int operation;\
	unsigned long addr;\
	unsigned long new_addr;\
	size_t len;\
	unsigned long new_len;\
	unsigned long prot;\
	unsigned long flags; \
	int from_nid;\
	int vma_operation_index;\
	int pgoff;\
	char path[512];
DEFINE_PCN_KMSG(vma_operation_t, VMA_OPERATION_FIELDS);

#define VMA_LOCK_FIELDS \
	int origin_nid; \
	int origin_pid; \
	int from_nid;\
	int vma_operation_index;
DEFINE_PCN_KMSG(vma_lock_t, VMA_LOCK_FIELDS);

#define VMA_ACK_FIELDS \
	int origin_nid; \
	int origin_pid; \
	int vma_operation_index;\
	unsigned long addr;
DEFINE_PCN_KMSG(vma_ack_t, VMA_ACK_FIELDS);

#define DATA_RESPONSE_FIELDS \
	int tgroup_home_cpu; \
	int tgroup_home_id;  \
	unsigned long address; \
	__wsum checksum; \
	long last_write;\
	int owner;\
	int vma_present; \
	unsigned long vaddr_start;\
	unsigned long vaddr_size;\
	pgprot_t prot; \
	unsigned long vm_flags; \
	unsigned long pgoff;\
	char path[512];\
	unsigned int data_size;\
	int diff;\
	int futex_owner; \
	char data;
DEFINE_PCN_KMSG(data_response_for_2_kernels_t,DATA_RESPONSE_FIELDS);

typedef struct _memory_struct {
	int tgroup_home_cpu;
	int tgroup_home_id;
	struct mm_struct* mm;

	struct task_struct *helper;

	// VMA operations
	char path[512];
	int operation;
	unsigned long addr;
	unsigned long new_addr;
	size_t len;
	unsigned long new_len;
	unsigned long prot;
	unsigned long pgoff;
	unsigned long flags;
	struct task_struct* waiting_for_main;
	struct task_struct* waiting_for_op;
	int arrived_op;
	int my_lock;

	unsigned char kernel_set[MAX_POPCORN_NODES];
	struct rw_semaphore kernel_set_sem;

	vma_operation_t *message_push_operation;
} memory_t;
/* Legacy definitions. Remove these quickly -----------------------*/

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
