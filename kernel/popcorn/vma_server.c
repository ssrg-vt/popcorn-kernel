/**
 * @file vma_server.c
 *
 * Popcorn Linux VMA server implementation
 * This work is an extension of David Katz MS Thesis, please refer to the
 * Thesis for further information about the algorithm.
 *
 * @author Sang-Hoon Kim, SSRG Virginia Tech 2017
 * @author Vincent Legout, Antonio Barbalace, SSRG Virginia Tech 2016
 * @author Ajith Saya, Sharath Bhat, SSRG Virginia Tech 2015
 * @author Marina Sadini, Antonio Barbalace, SSRG Virginia Tech 2014
 * @author Marina Sadini, SSRG Virginia Tech 2013
 */

/*
 * As David Katz thesis the concept of this server is to do consistent
 * modifications to the VMA list
 * The protocol is for N kernels
 * The performed operation is shown atomic to every thread of the same
 * application
 */

#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/kthread.h>

#include <linux/mman.h>
#include <linux/highmem.h>
#include <linux/ptrace.h>

#include <linux/elf.h>

#include <popcorn/types.h>
#include <popcorn/bundle.h>

#include "types.h"
#include "util.h"
#include "vma_server.h"
#include "wait_station.h"

enum vma_op_code {
	VMA_OP_NOP = 0,
	VMA_OP_MAP,
	VMA_OP_UNMAP,
	VMA_OP_PROTECT,
	VMA_OP_REMAP,
	VMA_OP_BRK,
	VMA_OP_MADVISE,
	VMA_OP_MAX,
};


static unsigned long map_difference(struct mm_struct *mm, struct file *file,
		unsigned long start, unsigned long end,
		unsigned long prot, unsigned long flags, unsigned long pgoff)
{
	unsigned long ret = start;
	unsigned long error;
	unsigned long populate = 0;
	struct vm_area_struct* vma;

	// go through ALL vma's, looking for interference with this space.
	for (vma = current->mm->mmap; start < end; vma = vma->vm_next) {
		if (vma == NULL || end <= vma->vm_start) {
			// We've reached the end of the list,  or the VMA is fully
			// above the region of interest
			error = do_mmap_pgoff(file, start, end - start,
					prot, flags, pgoff, &populate);
			VSPRINTK("  [%d] map0 %lx -- %lx @ %lx\n",
					current->pid, start, end, pgoff);

			if (error != start) {
				ret = VM_FAULT_SIGBUS;
			}
			break;
		} else if (start >= vma->vm_start && end <= vma->vm_end) {
			// the VMA fully encompases the region of interest
			// nothing to do
			break;
		} else if (start >= vma->vm_start
				&& start < vma->vm_end && end > vma->vm_end) {
			// the VMA includes the start of the region of interest
			// but not the end
			// advance start (no mapping to do)
			pgoff += ((vma->vm_end - start) >> PAGE_SHIFT);
			start = vma->vm_end;
		} else if (start < vma->vm_start
				&& vma->vm_start < end && end <= vma->vm_end) {
			// the VMA includes the end of the region of interest
			// but not the start

			error = do_mmap_pgoff(file, start, vma->vm_start - start,
					prot, flags, pgoff, &populate);
			VSPRINTK("  [%d] map1 %lx -- %lx, %lx\n",
					current->pid, start, vma->vm_start, pgoff);
			if (error != start) {
				ret = VM_FAULT_SIGBUS;;
			}
			break;
		} else if (start <= vma->vm_start && vma->vm_end <= end) {
			// the VMA is fully within the region of interest

			error = do_mmap_pgoff(file, start, vma->vm_start - start,
					prot, flags, pgoff, &populate);
			VSPRINTK("  [%d] map2 %lx -- %lx, %lx\n",
					current->pid, start, vma->vm_start, pgoff);
			if (error != start) {
				ret = VM_FAULT_SIGBUS;
				break;
			}

			// Then advance to the end of this vma
			pgoff += ((vma->vm_end - start) >> PAGE_SHIFT);
			start = vma->vm_end;
		}
	}

	BUG_ON(populate);

	return ret;
}



///////////////////////////////////////////////////////////////////////////////
// Legacy definitions. Remove these quickly

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


typedef struct _memory_struct {
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

static memory_t* find_memory_entry(int cpu, int id)
{
	return NULL;
}


typedef struct vma_op_answers {
	int origin_nid;
	int origin_pid;
	int responses;
	int expected_responses;
	int vma_operation_index;
	unsigned long address;
	struct task_struct *waiting;
	spinlock_t lock;
} vma_op_answers_t;

static vma_op_answers_t *vma_op_answer_alloc(struct task_struct * task, int index)
{
	vma_op_answers_t *acks = kzalloc(sizeof(*acks), GFP_ATOMIC);
	if (!acks) return NULL;

	acks->origin_nid = task->origin_nid;
	acks->origin_pid = task->origin_pid;
	acks->vma_operation_index = index;
	acks->waiting = task;
	acks->responses = 0;
	acks->expected_responses = 0;
	spin_lock_init(&acks->lock);
	// add_vma_ack_entry(acks);

	return acks;
}

static vma_lock_t *vma_lock_alloc(struct task_struct * task, int from_nid, int index)
{
	vma_lock_t* lock_message = kmalloc(sizeof(vma_lock_t), GFP_ATOMIC);

	lock_message->header.type = PCN_KMSG_TYPE_VMA_LOCK;
	lock_message->header.prio = PCN_KMSG_PRIO_NORMAL;

	lock_message->origin_nid = task->origin_nid;
	lock_message->origin_pid = task->origin_pid;
	lock_message->from_nid = from_nid;
	lock_message->vma_operation_index = index;

	return lock_message;
}

static vma_operation_t * vma_operation_alloc(struct task_struct * task, int op_id,
		unsigned long addr, unsigned long new_addr, int len, int new_len,
		unsigned long prot, unsigned long flags, int index)
{
	vma_operation_t* operation = kmalloc(sizeof(vma_operation_t), GFP_ATOMIC);

	operation->header.type = PCN_KMSG_TYPE_VMA_OP;
	operation->header.prio = PCN_KMSG_PRIO_NORMAL;
	operation->origin_nid = task->origin_nid;
	operation->origin_pid = task->origin_pid;
	operation->operation = op_id;
	operation->addr = addr;
	operation->new_addr = new_addr;
	operation->len = len;
	operation->new_len = new_len;
	operation->prot = prot;
	operation->flags = flags;
	operation->vma_operation_index = index;
	operation->from_nid = my_nid;
	operation->from_nid = my_nid;

	return operation;
}


static inline int vma_send_long_all(memory_t *entry, void *message, int size,
		struct task_struct *task, int max_distr_vma_op)
{
	int i;
	int acks = 0;

	// the list does not include the current processor group descirptor (TODO)
	for (i = 0; i < MAX_POPCORN_NODES; i++) {
		if (!get_popcorn_node_online(i) || i == my_nid) {
			continue;
		}
		if (task
			&& (task->mm->distr_vma_op_counter > max_distr_vma_op)
			&& (i == entry->message_push_operation->from_nid))
			continue;

		if (pcn_kmsg_send(i, message, size) != -1) {
			acks++;
		}
	}
	return acks;
}

// Legacy definitions. Remove these quickly
///////////////////////////////////////////////////////////////////////////////



/**
 * Heterogeneous binary support
 */
/* ajith - for file offset fetch. copied from fs/binfmt_elf.c*/

#if ELF_EXEC_PAGESIZE > PAGE_SIZE
#define ELF_MIN_ALIGN   ELF_EXEC_PAGESIZE
#else
#define ELF_MIN_ALIGN   PAGE_SIZE
#endif

/* Ajith - adding file offset parsing */
static unsigned long __get_file_offset(struct file *file, int start_addr)
{
	struct elfhdr elf_ex;
	struct elf_phdr *elf_eppnt = NULL, *elf_eppnt_start = NULL;
	int size, retval, i;

	retval = kernel_read(file, 0, (char *)&elf_ex, sizeof(elf_ex));
	if (retval != sizeof(elf_ex)) {
		printk("%s: ERROR in Kernel read of ELF file\n", __func__);
		retval = -1;
		goto out;
	}

	size = elf_ex.e_phnum * sizeof(struct elf_phdr);

	elf_eppnt = kmalloc(size, GFP_KERNEL);
	if (elf_eppnt == NULL) {
		printk("%s: ERROR: kmalloc failed in\n", __func__);
		retval = -1;
		goto out;
	}

	elf_eppnt_start = elf_eppnt;
	retval = kernel_read(file, elf_ex.e_phoff, (char *)elf_eppnt, size);
	if (retval != size) {
		printk("%s: ERROR: during kernel read of ELF file\n", __func__);
		retval = -1;
		goto out;
	}
	for (i = 0; i < elf_ex.e_phnum; i++, elf_eppnt++) {
		if (elf_eppnt->p_type != PT_LOAD) continue;

		printk("%s: Page offset for 0x%x 0x%lx 0x%lx\n", __func__,
				start_addr,
				(unsigned long)elf_eppnt->p_vaddr,
				(unsigned long)elf_eppnt->p_memsz);

		if ((start_addr >= elf_eppnt->p_vaddr) &&
				(start_addr <= (elf_eppnt->p_vaddr + elf_eppnt->p_memsz))) {
			retval = elf_eppnt->p_offset -
					(elf_eppnt->p_vaddr & (ELF_MIN_ALIGN - 1));
			break;
		}
		/*
		if ((elf_eppnt->p_flags & PF_R) && (elf_eppnt->p_flags & PF_X)) {
			printk("Coming to executable program load section\n");
			retval = elf_eppnt->p_offset -
					(elf_eppnt->p_vaddr & (ELF_MIN_ALIGN - 1));
			break;
		}
		*/
	}

out:
	if (elf_eppnt_start != NULL)
		kfree(elf_eppnt_start);

	return retval >> PAGE_SHIFT;
}


/*
 * THIS IS THE SERVER CODE
 * (not about server/client as Marina wrote that is home/not home kernel)
 * currently a workqueue
 */
struct vma_op_work {
	struct work_struct work;
	vma_operation_t* operation;
	memory_t* memory;
	int dying;
};

DECLARE_WAIT_QUEUE_HEAD(request_distributed_vma_op);

static void process_vma_op(struct work_struct* work)
{
	struct vma_op_work* vma_work = (struct vma_op_work*) work;
	vma_operation_t* operation = vma_work->operation;
	memory_t* memory = vma_work->memory;
	struct mm_struct* mm = memory->mm;

	// Coordinate the death of process
	if (vma_work->dying == 1) {
		unsigned long flags;

		memory->arrived_op = 1;
		lock_task_sighand(memory->helper, &flags);
		memory->helper->exit_code = 0; // sanghoon: was EXIT_FLUSHING
		unlock_task_sighand(memory->helper, &flags);

		wake_up_process(memory->helper);
		kfree(work);
		return;
	}

	PSPRINTK("Received vma operation from cpu %d for origin_nid %i "
			"origin_pid %i operation %i\n",
			operation->header.from_nid, operation->origin_nid,
			operation->origin_pid, operation->operation);

	down_write(&mm->mmap_sem);
	if (my_nid == operation->origin_nid) { //SERVER
		//if another operation is on going, it will be serialized after.
		//original code has concurrency errors -- this is similar to the client code (client code not in Marina's terms)
		// ONLY ONE CAN BE IN DISTRIBUTED OPERATION AT THE TIME ... WHAT ABOUT NESTED OPERATIONS?
		while (atomic_cmpxchg((atomic_t*)&(mm->distr_vma_op_counter), 0, 1) != 0) { //success is indicated by comparing RETURN with OLD (arch/x86/include/asm/cmpxchg.h)
			DEFINE_WAIT(wait);
			up_write(&mm->mmap_sem);

			prepare_to_wait(&request_distributed_vma_op, &wait, TASK_UNINTERRUPTIBLE);
			schedule();
			finish_wait(&request_distributed_vma_op, &wait);

			down_write(&mm->mmap_sem);
		}
		// here the distr_vma_op_counter is ours so let's check only the was_not_pushed
		BUG_ON(mm->was_not_pushed != 0 &&
			"ERROR: handling a new vma operation but has not-pushed one");

		mm->thread_op = memory->helper;

		if (operation->operation == VMA_OP_MAP
		    || operation->operation == VMA_OP_BRK) {
			mm->was_not_pushed++;
		}
		up_write(&mm->mmap_sem);

		//wake up the main thread to execute the operation locally
		memory->message_push_operation = operation;
		memory->addr = operation->addr;
		memory->len = operation->len;
		memory->prot = operation->prot;
		memory->new_addr = operation->new_addr;
		memory->new_len = operation->new_len;
		memory->flags = operation->flags;
		memory->pgoff = operation->pgoff;
		strcpy(memory->path, operation->path);
		memory->waiting_for_main = current;

		//This is the field check by the main thread
		//so it is the last one to be populated
		memory->operation = operation->operation;
		wake_up_process(memory->helper);

		// Wait until the worker process the request
		while (memory->operation != VMA_OP_NOP) {
			set_task_state(current, TASK_UNINTERRUPTIBLE);
			schedule();
			set_task_state(current, TASK_RUNNING);
		}

		down_write(&mm->mmap_sem);

		mm->distr_vma_op_counter--;
		BUG_ON(mm->distr_vma_op_counter != 0);

		if (operation->operation == VMA_OP_MAP
		    || operation->operation == VMA_OP_BRK) {
			mm->was_not_pushed--;
			BUG_ON(mm->was_not_pushed != 0);
		}
		mm->thread_op = NULL;

		up_write(&mm->mmap_sem);

		// I am done with the vma worker now. Wake up the next pending
		wake_up(&request_distributed_vma_op);

		pcn_kmsg_free_msg(operation);
		kfree(work);
		PSPRINTK("SERVER: %d\n", mm->vma_operation_index);
		PSPRINTK("SERVER: end requested operation\n");

		return;
	} else {
		struct task_struct *prev;
		PSPRINTK("CLIENT: Starting operation %i of index %i\n ",
				operation->operation, operation->vma_operation_index);
		//CLIENT

		//NOTE: the current->mm->distribute_sem is already held

		//MMAP and BRK are not pushed in the system
		//if I receive one of them I must have initiate it
		if (operation->operation == VMA_OP_MAP
		    || operation->operation == VMA_OP_BRK) {

			BUG_ON(memory->my_lock != 1 && "wrong distributed lock aquisition");
			BUG_ON(operation->from_nid != my_nid &&
				"Server pushed me operation of cpu operation->from_nid");
			BUG_ON(memory->waiting_for_op == NULL &&
				"Received a push operation started by me but nobody is waiting");

			memory->addr = operation->addr;
			memory->arrived_op = 1;

			PSPRINTK("CLIENT: %d\n", mm->vma_operation_index);
			PSPRINTK("CLIENT: Operation %i started by a local thread pid %d\n ",
					operation->operation, memory->waiting_for_op->pid);
			up_write(&mm->mmap_sem);

			wake_up_process(memory->waiting_for_op);

			pcn_kmsg_free_msg(operation);
			kfree(work);
			return ;
		}

		//I could have started the operation...check!
		if (operation->from_nid == my_nid) {
			BUG_ON(memory->my_lock != 1 &&
				"ERROR: wrong distributed lock aquisition");
			BUG_ON(memory->waiting_for_op == NULL &&
				"ERROR:received a push operation started by me "
				"but nobody is waiting");

			if (operation->operation == VMA_OP_REMAP)
				memory->addr = operation->new_addr;

			memory->arrived_op = 1;
			PSPRINTK("CLIENT: Operation %i started by a local thread pid %d index %d\n ",
					operation->operation, memory->waiting_for_op->pid, mm->vma_operation_index);
			up_write(&mm->mmap_sem);

			wake_up_process(memory->waiting_for_op);
			pcn_kmsg_free_msg(operation);
			kfree(work);
			return ;
		}

		PSPRINTK("CLIENT Pushed operation started by somebody else\n");
		if (operation->addr < 0) {
			printk("WARN: server sent me and error\n");
			pcn_kmsg_free_msg(operation);
			kfree(work);
			return ;
		}

		mm->distr_vma_op_counter++;
		prev = mm->thread_op;

		while (memory->helper == NULL) // waiting for main to be allocated (?)
			schedule();

		mm->thread_op = memory->helper;
		up_write(&mm->mmap_sem);

		//wake up the main thread to execute the operation locally
		memory->addr = operation->addr;
		memory->len = operation->len;
		memory->prot = operation->prot;
		memory->new_addr = operation->new_addr;
		memory->new_len = operation->new_len;
		memory->flags = operation->flags;

		//the new_addr sent by the server is fixed
		if (operation->operation == VMA_OP_REMAP)
			memory->flags |= MREMAP_FIXED;

		memory->waiting_for_main = current;
		memory->operation = operation->operation;

		wake_up_process(memory->helper);

		PSPRINTK("CLIENT: woke up the main5\n");

		while (memory->operation != VMA_OP_NOP) {
			set_task_state(current, TASK_UNINTERRUPTIBLE);
			schedule();
			set_task_state(current, TASK_RUNNING);
		}

		down_write(&mm->mmap_sem);
		memory->waiting_for_main = NULL;
		mm->thread_op = prev;
		mm->distr_vma_op_counter--;

		mm->vma_operation_index++;

		if (memory->my_lock != 1) {
			VSPRINTK("Released distributed lock\n");
			up_write(&mm->distribute_sem);
		}

		VSPRINTK("INFO: %d ENDING OP\n", mm->vma_operation_index);
		up_write(&mm->mmap_sem);

		wake_up(&request_distributed_vma_op);
		pcn_kmsg_free_msg(operation);
		kfree(work);
		return ;
	}
}

static void process_vma_lock(struct work_struct* _work)
{
	struct pcn_kmsg_work *work = (struct pcn_kmsg_work *)_work;
	vma_lock_t* lock = work->msg;

	memory_t* entry = find_memory_entry(lock->origin_nid, lock->origin_pid);
	vma_ack_t *ack_to_server;
	if (entry != NULL) {
		down_write(&entry->mm->distribute_sem);
		VSPRINTK("Acquired distributed lock\n");
		if (lock->from_nid == my_nid)
			entry->my_lock = 1;
	}

	ack_to_server = kmalloc(sizeof(vma_ack_t), GFP_ATOMIC);
	BUG_ON(!ack_to_server);

	ack_to_server->origin_nid = lock->origin_nid;
	ack_to_server->origin_pid = lock->origin_pid;
	ack_to_server->vma_operation_index = lock->vma_operation_index;
	ack_to_server->header.type = PCN_KMSG_TYPE_VMA_ACK;
	ack_to_server->header.prio = PCN_KMSG_PRIO_NORMAL;

	pcn_kmsg_send(lock->origin_nid, ack_to_server, sizeof(*ack_to_server));

	kfree(ack_to_server);
	pcn_kmsg_free_msg(lock);
	kfree(work);
	return;
}


static int handle_vma_ack(struct pcn_kmsg_message* inc_msg)
{
	vma_ack_t* ack = (vma_ack_t*) inc_msg;
	vma_op_answers_t* ack_holder;
	unsigned long flags;
	struct task_struct* task_to_wake_up = NULL;

	VSPRINTK("Vma ack received from cpu %d\n", ack->header.from_nid);
	ack_holder = NULL; // find_vma_ack_entry(ack->origin_nid, ack->origin_pid);
	if (ack_holder) {
		spin_lock_irqsave(&(ack_holder->lock), flags);

		ack_holder->responses++;
		ack_holder->address = ack->addr;

		if (ack_holder->vma_operation_index == -1)
			ack_holder->vma_operation_index = ack->vma_operation_index;
		else if (ack_holder->vma_operation_index != ack->vma_operation_index)
			printk("ERROR: receiving an ack vma for a different operation index");

		if (ack_holder->responses >= ack_holder->expected_responses)
			task_to_wake_up = ack_holder->waiting;

		spin_unlock_irqrestore(&(ack_holder->lock), flags);

		if (task_to_wake_up)
			wake_up_process(task_to_wake_up);
	}

	pcn_kmsg_free_msg(inc_msg);
	return 1;
}


/**
 * This function coordinates the end of a distributed VMA operation among
 * different Popcorn Linux kernels.
 *
 * @return The function doesn't return an error but it can either print an error
 *         message on the kernel log or throw a kernel bug.
 *
 *
 * which are the locks help on entering this function?
 * we are holding the mm->mmap_sem in write --- we are exiting with mmap_sem in write (hold) WE ARE NOT CHANGING IT
 * getting in we have also mm->distribute_sem getting out we are releasing it if there are no operations anymore
 * we are in down_read on the &entry->kernel_set_sem
 *
 * (check better but we are basically releasing both
 */
void end_distribute_operation(int operation, long start_ret, unsigned long addr)
{
	memory_t *entry;
	if (current->mm->distribute_unmap == 0)
		return;

	//printk("INFO: Ending distributed vma operation %i pid %d counter %d\n",
	//	operation,current->pid, current->mm->distr_vma_op_counter);
	if (current->mm->distr_vma_op_counter <= 0
	    || (!current->is_vma_worker && current->mm->distr_vma_op_counter > 2)
	    || ( current->is_vma_worker && current->mm->distr_vma_op_counter > 3))
		printk("ERROR: exiting from a distributed vma operation "
				"with distr_vma_op_counter = %i\n",
				current->mm->distr_vma_op_counter);

	// decrement the counter for nested operations
	current->mm->distr_vma_op_counter--;

	if (operation == VMA_OP_MAP || operation == VMA_OP_BRK) {
		if (current->mm->was_not_pushed <= 0)
			printk("ERROR: exiting from a mapping operation "
					"with was_not_pushed = %i\n",
					current->mm->was_not_pushed);
		current->mm->was_not_pushed--;
	}

	entry = find_memory_entry(current->origin_nid, current->origin_pid);
	BUG_ON(!entry && "Cannot find message to send in exit operation");

#define VMA_OP_SAVE (-70)

	if (start_ret == VMA_OP_SAVE) {
		int err;
		BUG_ON(my_nid != current->origin_nid &&
			"asked asking for saving address from a client");
		BUG_ON(!entry->message_push_operation &&
			"Cannot find message to send in exit operation");

		//now I have the new address I can send the message
		if (entry->message_push_operation != NULL) {
			switch (operation) {
			case VMA_OP_MAP:
			case VMA_OP_BRK:
				BUG_ON(!current->is_vma_worker &&
					"server not worker asked to save operation");
				entry->message_push_operation->addr = addr;
				break;
			case VMA_OP_REMAP:
				entry->message_push_operation->new_addr = addr;
				break;
			default:
				BUG_ON("asked from a wrong operation");
				break;
			}
			up_write(&current->mm->mmap_sem);

			switch (operation) {
			case VMA_OP_MAP:
			case VMA_OP_BRK:
				err = pcn_kmsg_send(
						entry->message_push_operation->from_nid,
						entry->message_push_operation, sizeof(vma_operation_t));
				PSPRINTK("INFO: operation %d sent to cpu %d\n",
						operation, entry->message_push_operation->from_nid);
				break;
			case VMA_OP_REMAP:
				PSPRINTK("INFO: sending operation %d to all\n", operation);
				vma_send_long_all(entry, entry->message_push_operation,
						sizeof(vma_operation_t), 0, 0);
				break;
			}

			down_write(&current->mm->mmap_sem);
			if (!current->is_vma_worker) {
				kfree(entry->message_push_operation);
				entry->message_push_operation = NULL;
			}
		}
	}

/*****************************************************************************/
/* Here the message has been sent already in case of VMA_OP_SAVE             */
	if (current->mm->distr_vma_op_counter == 0) { // there are no nested operations (how I can be sure no one else it changing this here?
		current->mm->thread_op = NULL;
		entry->my_lock = 0;

		if (!(operation == VMA_OP_MAP || operation == VMA_OP_BRK)) { // operation is neither MAP nor BRK
			VSPRINTK("Incrementing vma_operation_index\n");
			current->mm->vma_operation_index++;
		}

		PSPRINTK("Releasing distributed lock\n");
		up_write(&current->mm->distribute_sem);

		if ( my_nid == current->origin_nid && !(operation == VMA_OP_MAP || operation == VMA_OP_BRK) ) {
			up_read(&entry->kernel_set_sem);
		}
		wake_up(&request_distributed_vma_op);
	} else { // there are nested operations --- not all situations are handled
		if (current->mm->distr_vma_op_counter == 1
		    && my_nid == current->origin_nid && current->is_vma_worker) {

			if (!(operation == VMA_OP_MAP || operation == VMA_OP_BRK)){
				VSPRINTK("Incrementing vma_operation_index\n");
				current->mm->vma_operation_index++;

				up_read(&entry->kernel_set_sem);
			}

			PSPRINTK("Releasing distributed lock\n");
			up_write(&current->mm->distribute_sem);
		} else {
			if (!(current->mm->distr_vma_op_counter == 1
			      && my_nid != current->origin_nid && current->is_vma_worker)) {

				//nested operation
				WARN(operation != VMA_OP_UNMAP, "Exiting from a nest operation");

				//nested operation do not release the lock
				PSPRINTK("incrementing vma_operation_index\n");
				current->mm->vma_operation_index++;
			}
		}
	}
	PSPRINTK("%d\n", current->mm->vma_operation_index);
}


/**
 * This function takes a distributed lock for a VMA operation, and triggers
 * the same operation among different Popcorn Linux kernels.
 *
 * @return Returns either a valid memory address for the distributed operation
 *         or an error code in case of failures.
 *
 *
 * Marina: nesting is handled manually (?)
 *
 * Operations can be nested-called.
 * MMAP->UNMAP
 * BR->UNMAP
 * MPROT->/
 * UNMAP->/
 * MREMAP->UNMAP
 * =>only UNMAP can be nested-called
 *
 * If this is an unmap nested-called by an operation pushed in the system,
 * skip the distribution part.
 *
 * If this is an unmap nested-called by an operation not pushed in the system,
 * and I am the server, push it in the system.
 *
 * If this is an unmap nested-called by an operation not pushed in the system,
 * and I am NOT the server, it is an error. The server should have pushed that
 * unmap
 * Before, if I am executing it again something is wrong.
 */
/* I assume that down_write(&mm->mmap_sem) is held
 * There are two different protocols:
 * mmap and brk need to only contact the server,
 * all other operations (remap, mprotect, unmap) need that the server pushes
 * it in the system
 */
long start_distribute_operation(int operation, unsigned long addr, size_t len,
			unsigned long prot, unsigned long new_addr, unsigned long new_len,
			unsigned long flags, struct file *file, unsigned long pgoff)
{
	long ret = addr;
	bool server = !current->at_remote;

	// set default return value
	if (server)
		ret = VMA_OP_SAVE - 1;
	else if (operation == VMA_OP_REMAP)
		ret = new_addr;

	/* All the operation pushed by the server are executed as not distributed in clients*/
	if (current->mm->distribute_unmap == 0) {
		return ret;
	}

	/* only server can have legal distributed nested operations */
	if ((current->mm->distr_vma_op_counter > 0) &&
			(current->mm->thread_op == current)) {
		PSPRINTK("Recursive operation\n");
		if (!server
			|| (!current->is_vma_worker && current->mm->distr_vma_op_counter > 1)
			|| (!current->is_vma_worker && operation != VMA_OP_UNMAP)) {
			BUG_ON("ERROR: invalid nested vma operation");
			return -EPERM;
		}

		/* the main executes the operations for the clients
		 * distr_vma_op_counter is already increased when it start the operation*/
		if (current->is_vma_worker) {
			VSPRINTK("I am the worker, so it maybe not a real "
					"recursive operation..."); // what??????

			if (current->mm->distr_vma_op_counter < 1 // who change the value in the meantime?
				|| current->mm->distr_vma_op_counter > 2
				|| (current->mm->distr_vma_op_counter == 2 && operation != VMA_OP_UNMAP)) {
				printk("ERROR: invalid nested vma operation in main server, "
						"operation %d (counter %d)\n",
						operation, current->mm->distr_vma_op_counter);
				return -EPERM;
			}
			//current->mm->distr_vma_op_counter++;

			if (current->mm->distr_vma_op_counter++ == 2) {
				VSPRINTK("Recursive operation for the main\n");
				/* in this case is a nested operation on main
				 * if the previous operation was a pushed operation
				 * do not distribute it again*/
				if (current->mm->was_not_pushed == 0) {
					VSPRINTK("Don't distribute again, return!\n");
					return ret;
				}
				current->mm->distr_vma_op_counter++;
			}
		} else {
			if (current->mm->was_not_pushed == 0) {
				current->mm->distr_vma_op_counter++; // this is decremented in end_distribute_operation
				VSPRINTK("Don't distribute again, return!\n");
				return ret;
			}
			current->mm->distr_vma_op_counter++;
		}
		goto start;
	}

	/* I did not start an operation, but another thread maybe did...
	 * => no concurrent operations of the same process on the same kernel*/
	BUG_ON(current->mm->distr_vma_op_counter < 0);

	// ONLY ONE CAN BE IN DISTRIBUTED OPERATION AT THE TIME...
	// WHAT ABOUT NESTED OPERATIONS?
	while (atomic_cmpxchg((atomic_t*)&(current->mm->distr_vma_op_counter), 0, 1) != 0) {
		DEFINE_WAIT(wait);
		up_write(&current->mm->mmap_sem);

		prepare_to_wait(
				&request_distributed_vma_op, &wait,TASK_UNINTERRUPTIBLE);
		printk("LOCK: %d already started a distributed operation ",
				current->mm->thread_op->pid);
		schedule();
		finish_wait(&request_distributed_vma_op, &wait);

		down_write(&current->mm->mmap_sem);
	}

start:
	current->mm->thread_op = current;

///////////////////////////////////////////////////////////////////////////////
// start distributed operation
///////////////////////////////////////////////////////////////////////////////

	if (operation == VMA_OP_MAP || operation == VMA_OP_BRK) {
		current->mm->was_not_pushed++;
	}

//SERVER MAIN (counter <= 2 <<< recursive) ////////////////////////////////////
	if (server) {
		if (current->is_vma_worker && current->mm->distr_vma_op_counter <= 2) {
			/* I am the main thread=> a client asked me to do an operation. */
			int index = current->mm->vma_operation_index;
			memory_t* entry;
			PSPRINTK("SERVER MAIN: starting operation %d, current index %d\n",
					operation, index);

			up_write(&current->mm->mmap_sem);
			entry = find_memory_entry(current->origin_nid, current->origin_pid);
			BUG_ON((entry == NULL || entry->message_push_operation == NULL) &&
				"Mapping disappeared or cannot find message to update");

/*****************************************************************************/
/* Locking and Acking                                                        */
/*****************************************************************************/
			{
				vma_op_answers_t *acks;
				//First: send a message to everybody to acquire the lock to block page faults
				vma_lock_t* lock_message = vma_lock_alloc(current,
						entry->message_push_operation->from_nid, index);
				acks = vma_op_answer_alloc(current, index);

				/*
				 * Partial replication: mmap and brk need to communicate only between server and one client
				 */
				if (operation == VMA_OP_MAP || operation == VMA_OP_BRK) {
					pcn_kmsg_send(entry->message_push_operation->from_nid,
							lock_message, sizeof(vma_lock_t));
					acks->expected_responses++;
				} else {
					down_read(&entry->kernel_set_sem);
					acks->expected_responses = vma_send_long_all(entry,
							lock_message, sizeof(vma_lock_t), current, 2);
				}

				while (acks->expected_responses != acks->responses) {
					set_task_state(current, TASK_UNINTERRUPTIBLE);
					schedule();
					set_task_state(current, TASK_RUNNING);
				}
				PSPRINTK("SERVER MAIN: Received all ack to lock\n");

				//remove_vma_ack_entry(acks);
				kfree(acks);
				kfree(lock_message);
			}

			/*I acquire the lock to block page faults too
			 *Important: this should happen before sending the push message or executing the operation*/
			if (current->mm->distr_vma_op_counter == 2) {
				down_write(&current->mm->distribute_sem);
				VSPRINTK("local distributed lock acquired\n");
			}
/*****************************************************************************/
/* Locking and Acking --- END ---                                            */
/*****************************************************************************/

			entry->message_push_operation->vma_operation_index = index;

			/* Third: push the operation to everybody
			 * If the operation was a mmap,brk or remap without fixed parameters, I cannot let other kernels
			 * locally choose where to remap it =>
			 * I need to push what the server choose as parameter to the other an push the operation with
			 * a fixed flag.
			 * */
			if (operation == VMA_OP_UNMAP
				|| operation == VMA_OP_PROTECT
				|| (operation == VMA_OP_REMAP && (flags & MREMAP_FIXED))) {
				PSPRINTK("INFO: SERVER MAIN we can execute the operation"
						"in parallel! %d %lx %lx\n",
						operation, flags, flags & MREMAP_FIXED);

				vma_send_long_all(entry, entry->message_push_operation,
						sizeof(vma_operation_t), 0, 0);
				down_write(&current->mm->mmap_sem);
				return ret;
			} else {
				PSPRINTK("INFO: SERVER MAIN going to execute the operation"
						"locally %d\n",
						operation);

				down_write(&current->mm->mmap_sem);
				return VMA_OP_SAVE;
			}
		} else {
//SERVER not main//////////////////////////////////////////////////////////////
			int index;
			memory_t *entry;
			vma_operation_t* operation_to_send;

			if (current->is_vma_worker != 0) ///ERROR IF I AM NOT MAIN - do this check because there can be a possibility of >2 counter
				printk("WARN?ERROR: Server not main operation "
						"but curr->is_vma_worker is %d\n",
						current->is_vma_worker);

			PSPRINTK("SERVER NOT MAIN starting operation %d for pid %d "
					"current index is %d\n",
					operation, current->pid, current->mm->vma_operation_index);

			switch (operation) {
			case VMA_OP_MAP:
			case VMA_OP_BRK:
				//if I am the server, mmap and brk can be executed locally
				VSPRINTK("Pure local operation!\n");
				//Note: the order in which locks are taken is important
				up_write(&current->mm->mmap_sem);

				down_write(&current->mm->distribute_sem);
				VSPRINTK("Distributed lock acquired\n");
				down_write(&current->mm->mmap_sem);

				return ret;
			default:
				break;
			}

			//new push-operation
			PSPRINTK("Push operation!\n");
			index = current->mm->vma_operation_index;
			PSPRINTK("current index is %d\n", index);

			/*Important: while I am waiting for the acks to the LOCK message
			 * mmap_sem have to be unlocked*/
			up_write(&current->mm->mmap_sem);
			entry = find_memory_entry(current->origin_nid, current->origin_pid);
			BUG_ON(!entry);


/*****************************************************************************/
/* Locking and Acking                                                        */
/*****************************************************************************/
			{
				/*First: send a message to everybody to acquire the lock to block page faults*/
				vma_op_answers_t *acks = vma_op_answer_alloc(current, index);
				vma_lock_t* lock_message = vma_lock_alloc(current, my_nid, index);

	//ANTONIOB: why here we are not distinguish between different type of operations?
				down_read(&entry->kernel_set_sem);
				acks->expected_responses = vma_send_long_all(
						entry, lock_message, sizeof(vma_lock_t), 0, 0);

				/*Second: wait that everybody acquire the lock, and acquire it locally too*/
				while (acks->expected_responses != acks->responses) {
					set_task_state(current, TASK_UNINTERRUPTIBLE);
					schedule();
					set_task_state(current, TASK_RUNNING);
				}
				PSPRINTK("SERVER NOT MAIN: Received all ack to lock\n");

				//remove_vma_ack_entry(acks);
				kfree(acks);
				kfree(lock_message);
			}

			/*I acquire the lock to block page faults too
			 *Important: this should happen before sending the push message or executing the operation*/
			if (current->mm->distr_vma_op_counter == 1) {
				down_write(&current->mm->distribute_sem);
				VSPRINTK("Distributed lock acquired locally\n");
			}
/*****************************************************************************/
/* Locking and Acking --- END---                                             */
/*****************************************************************************/

			operation_to_send = vma_operation_alloc(current, operation,
					addr, new_addr, len, new_len, prot, flags, index);

			/* Third: push the operation to everybody
			 * If the operation was a remap without fixed parameters, I cannot let other kernels
			 * locally choose where to remap it =>
			 * I need to push what the server choose as parameter to the other an push the operation with
			 * a fixed flag.
			 * */
			if (!(operation == VMA_OP_REMAP) || (flags & MREMAP_FIXED)) {
				PSPRINTK("INFO: SERVER NOT MAIN sending done for operation, "
						"we can execute the operation in parallel! %d\n",
						operation);

				vma_send_long_all(
						entry, operation_to_send, sizeof(vma_operation_t), 0, 0);
				kfree(operation_to_send);
				down_write(&current->mm->mmap_sem);
				return ret;
			} else {
				PSPRINTK("INFO: SERVER NOT MAIN "
						"going to execute the operation locally %d\n",
						operation);
				entry->message_push_operation = operation_to_send;

				down_write(&current->mm->mmap_sem);
				return VMA_OP_SAVE;
			}
		}
	}
/*****************************************************************************/
/* client (only one case)                                                    */
/*****************************************************************************/
	else {
		vma_operation_t* operation_to_send;
		memory_t *entry;
		int error;

		PSPRINTK("INFO: CLIENT starting operation %i for pid %d "
				"current index is%d\n",
				operation, current->pid, current->mm->vma_operation_index);

		/*First: send the operation to the server*/
		operation_to_send = vma_operation_alloc(current, operation,
				addr, new_addr, len, new_len, prot, flags, -1);

		operation_to_send->pgoff = pgoff;
		operation_to_send->path[0] = '\0';
		get_file_path(file, operation_to_send->path,
				sizeof(operation_to_send->path));

		/*In this case the server will eventually send me the push operation.
		 *Differently from a not-started-by-me push operation, it is not the main thread that has to execute it,
		 *but this thread has.
		 */
		entry = find_memory_entry(current->origin_nid, current->origin_pid);
		if (entry) {
			if (entry->waiting_for_op != NULL) {
				printk("ERROR: Somebody is already waiting for an op "
						"(cpu %d id %d)\n",
						current->origin_nid, current->origin_pid);
				kfree(operation_to_send);
				ret = -EPERM;
				goto out;
			}
			entry->waiting_for_op = current;
			entry->arrived_op = 0;
		} else {
			printk("ERROR: Mapping disappeared, "
					"cannot wait for push op (cpu %d id %d)\n",
					current->origin_nid, current->origin_pid);
			kfree(operation_to_send);
			ret = -EPERM;
			goto out;
		}

		up_write(&current->mm->mmap_sem);
		//send the operation to the server
		error = pcn_kmsg_send(current->origin_nid,
					   operation_to_send, sizeof(vma_operation_t));

		/*Second: the server will send me a LOCK message... another thread will handle it...*/
		/*Third: wait that the server push me the operation*/
		while (entry->arrived_op == 0) {
			set_task_state(current, TASK_UNINTERRUPTIBLE);
			schedule();
			set_task_state(current, TASK_RUNNING);
		}
		PSPRINTK("My operation finally arrived pid %d vma operation %d\n",
				current->pid,current->mm->vma_operation_index);

		/*Note, the distributed lock already has been acquired*/
		down_write(&current->mm->mmap_sem);

		if (current->mm->thread_op != current) {
			printk(	"ERROR: waking up to locally execute a vma operation "
					"started by me, but thread_op s not me\n");
			kfree(operation_to_send);
			ret = -EPERM;
			goto out_dist_lock;
		}

		if (operation == VMA_OP_REMAP
			|| operation == VMA_OP_MAP
		    || operation == VMA_OP_BRK) {
			ret = entry->addr;
			if (entry->addr < 0) {
				printk("WARN: Received error %lx from the server for "
						"operation %d\n", ret,operation);
				goto out_dist_lock;
			}
		}

		entry->waiting_for_op = NULL;
		kfree(operation_to_send);
		return ret;
	}

out_dist_lock:
	up_write(&current->mm->distribute_sem);
	PSPRINTK("Released distributed lock from out_dist_lock %d\n",
			current->mm->vma_operation_index);

out:
	current->mm->distr_vma_op_counter--;
	current->mm->thread_op = NULL;
	if (operation == VMA_OP_MAP || operation == VMA_OP_BRK) {
		current->mm->was_not_pushed--;
	}

	wake_up(&request_distributed_vma_op);
	return ret;
}



/*********************************************************************
 * NEW and CLEAN IMPLEMENTATOIN for Popcorn Rack
 *********************************************************************/

static vma_op_request_t *__alloc_vma_op_request(enum vma_op_code opcode, struct wait_station **ws)
{
	vma_op_request_t *req = kmalloc(sizeof(*req), GFP_KERNEL);
	*ws = get_wait_station(current);

	req->header.type = PCN_KMSG_TYPE_VMA_OP_REQUEST;
	req->header.prio = PCN_KMSG_PRIO_NORMAL;

	req->origin_pid = current->origin_pid,
	req->remote_nid = my_nid,
	req->remote_pid = current->pid,
	req->remote_ws = (*ws)->id,
	req->operation = opcode;

	return req;
}


unsigned long vma_server_mmap_remote(struct file *file,
		unsigned long addr, unsigned long len,
		unsigned long prot, unsigned long flags, unsigned long pgoff)
{
	struct wait_station *ws;
	unsigned long ret = 0;
	vma_op_request_t *req = __alloc_vma_op_request(VMA_OP_MAP, &ws);

	vma_op_response_t *res;

	req->addr = addr;
	req->len = len;

	req->prot = prot;
	req->flags = flags;
	req->pgoff = pgoff;
	get_file_path(file, req->path, sizeof(req->path));

	VSPRINTK("\n### VMA mmap [%d] %lx - %lx\n", current->pid,
			addr, addr + len);
	if (req->path[0] != '\0') {
		VSPRINTK("  [%d] %s\n", current->pid, req->path);
	}

	pcn_kmsg_send(current->origin_nid, req, sizeof(*req));
	res = wait_at_station(ws);
	put_wait_station(ws);
	BUG_ON(res->operation != req->operation);

	if (res->ret) {
		ret = -EINVAL;
		goto out_free;
	}

	VSPRINTK("  [%d] <-- %lx %lx -- %lx\n", current->pid,
			addr, res->addr, res->addr + res->len);

	down_write(&current->mm->mmap_sem);
	ret = map_difference(current->mm, file, res->addr, res->addr + res->len,
			prot, flags, pgoff);
	up_write(&current->mm->mmap_sem);

out_free:
	kfree(req);
	pcn_kmsg_free_msg(res);

	return ret;
}

int vma_server_munmap_remote(unsigned long start, size_t len)
{
	struct wait_station *ws;
	vma_op_request_t *req = __alloc_vma_op_request(VMA_OP_UNMAP, &ws);
	vma_op_response_t *res;

	req->addr = start;
	req->len = len;

	VSPRINTK("\n### VMA munmap [%d] %lx %lx\n", current->pid, start, len);

	pcn_kmsg_send(current->origin_nid, req, sizeof(*req));
	res = wait_at_station(ws);
	put_wait_station(ws);
	BUG_ON(res->operation != req->operation);

	down_write(&current->mm->mmap_sem);
	up_write(&current->mm->mmap_sem);

	kfree(req);
	pcn_kmsg_free_msg(res);

	return -EINVAL;
}

int vma_server_munmap_origin(unsigned long start, size_t len)
{
	VSPRINTK("\n### VMA munmap [%d] %lx %lx\n", current->pid, start, len);
	return -EINVAL;
}

int vma_server_brk_remote(unsigned long brk)
{
	VSPRINTK("\n### VMA brk [%d] %lx\n", current->pid, brk);
	return -EINVAL;
}

int vma_server_madvise_remote(unsigned long start, size_t len, int behavior)
{
	VSPRINTK("\n### VMA madvise [%d] %lx %lx %d\n", current->pid,
			start, len, behavior);
	return -EINVAL;
}

int vma_server_mprotect_remote(unsigned long start, size_t len, unsigned long prot)
{
	VSPRINTK("\n### VMA mprotect [%d] %lx %lx %lx\n", current->pid,
			start, len, prot);
	return -EINVAL;
}

int vma_server_mremap_remote(unsigned long addr, unsigned long old_len,
		unsigned long new_len, unsigned long flags, unsigned long new_addr)
{
	VSPRINTK("\n### VMA mremap [%d] %lx %lx %lx %lx %lx\n", current->pid,
			addr, old_len, new_len, flags, new_addr);
	return -EINVAL;
}


/**
 * VMA worker
 *
 * We do this stupid thing because functions related to meomry mapping operate
 * on "current". Thus, we need mmap/munmap/madvise in our process
 */
static void __reply_vma_op(vma_op_request_t *req, int ret)
{
	vma_op_response_t res = {
		.header = {
			.type = PCN_KMSG_TYPE_VMA_OP_RESPONSE,
			.prio = PCN_KMSG_PRIO_NORMAL,
		},

		.origin_pid = current->pid,
		.origin_nid = my_nid,
		.remote_pid = req->remote_pid,
		.remote_ws = req->remote_ws,
		.operation = req->operation,
	};

	res.ret = ret;
	res.addr = req->addr;
	res.len = req->len;

	pcn_kmsg_send(req->remote_nid, &res, sizeof(res));
}


static vma_op_request_t *__get_pending_vma_op(struct remote_context *rc)
{
	struct work_struct *work = NULL;
	vma_op_request_t *req;

	wait_for_completion_timeout(&rc->vma_works_ready, 10 * HZ);

	spin_lock(&rc->vma_works_lock);
	if (!list_empty(&rc->vma_works)) {
		work = list_first_entry(&rc->vma_works, struct work_struct, entry);
		list_del(&work->entry);
	}
	spin_unlock(&rc->vma_works_lock);

	if (!work) return NULL;

	req = ((struct pcn_kmsg_work *)work)->msg;
	kfree(work);

	return req;
}

void vma_worker_main(struct remote_context *rc, const char *at)
{
	struct mm_struct *mm = get_task_mm(current);
	might_sleep();

	PSPRINTK("%s [%d] at %s\n", __func__, current->pid, at);

	while (!kthread_should_stop()) {
		vma_op_request_t *req;
		int ret;

		if (!(req = __get_pending_vma_op(rc))) continue;

		printk("\n#### VMA_WORKER [%d] %d - %lx %lx\n", current->pid,
				req->operation, req->addr, req->len);

		switch (req->operation) {
		case VMA_OP_MAP: {
			unsigned long populate = 0;
			unsigned long raddr;
			struct file *f = NULL;

			if (req->path[0] != '\0')
				f = filp_open(req->path, O_RDONLY | O_LARGEFILE, 0);

			if (IS_ERR(f)) {
				printk("  [%d] Cannot open %s\n", current->pid, req->path);
				break;
			}
			down_write(&mm->mmap_sem);
			raddr = do_mmap_pgoff(f, req->addr, req->len, req->prot,
					req->flags, req->pgoff, &populate);
			up_write(&mm->mmap_sem);
			if (populate) mm_populate(raddr, populate);

			if (IS_ERR_VALUE(raddr)) {
				ret = raddr;
			} else {
				ret = 0;
			}

			req->addr = raddr;
			__reply_vma_op(req, ret);

			if (f) filp_close(f, NULL);
			break;
		}
		case VMA_OP_UNMAP:
			__replay_vma_op(req, 0);
			break;
		case VMA_OP_PROTECT:
		case VMA_OP_REMAP:
		case VMA_OP_BRK:
		case VMA_OP_MADVISE:
			break;
		default:
			WARN_ON("NO valid VMA operation");
		}
		/*
		extern long madvise_remove(struct vm_area_struct *vma,
			struct vm_area_struct **prev, unsigned long start, unsigned long end);
		extern int kernel_mprotect(unsigned long start, size_t len, unsigned long prot);
		extern long kernel_mremap(unsigned long addr, unsigned long old_len,
			unsigned long new_len, unsigned long flags, unsigned long new_addr);

		switch (req->operation) {
		case VMA_OP_UNMAP:
			down_write(&mm->mmap_sem);
			ret = do_munmap(mm, req->addr, req->len);
			up_write(&mm->mmap_sem);
			break;

		case VMA_OP_MADVISE: {
			// this is only for MADV_REMOVE (thus write is 0)
			struct vm_area_struct *pvma;
			struct vm_area_struct *vma = find_vma(mm, req->addr);
			down_read(&(mm->mmap_sem));
			ret = madvise_remove(vma, &pvma,
					req->addr, (req->addr + req->len));
			up_read(&(mm->mmap_sem));
			break;
		}

		case VMA_OP_PROTECT:
			ret = kernel_mprotect(req->addr, req->len, req->prot);
			break;

		case VMA_OP_REMAP:
			// remap calls unmap --- thus is a nested operation ...
			down_write(&mm->mmap_sem);
			ret = kernel_mremap(req->addr, req->len, req->new_len, 0, req->new_addr);
			up_write(&mm->mmap_sem);
			break;

		case VMA_OP_BRK:
			down_write(&mm->mmap_sem);
			ret = do_brk(req->addr, req->len);
			up_write(&mm->mmap_sem);
			break;

		case VMA_OP_MAP: {
			struct file *f = NULL;
			unsigned long populate = 0;
			if (req->path[0] != '\0') {
				f = filp_open(req->path, O_RDONLY | O_LARGEFILE, 0);
				if (IS_ERR(f)) {
					printk("ERROR: cannot open file to map\n");
					break;
				}
			}
			down_write(&mm->mmap_sem);
			ret = do_mmap_pgoff(f, req->addr, req->len, req->prot,
						req->flags, req->pgoff, &populate);
			up_write(&mm->mmap_sem);

			if (f) filp_close(f, NULL);
			break;
		}
		*/
		pcn_kmsg_free_msg(req);
	}
	mmput(mm);

	printk("%s [%d] %s exited\n", __func__, current->pid, at);

	return;
}

static void process_vma_op_request(struct work_struct *_work)
{
	struct pcn_kmsg_work *work = (struct pcn_kmsg_work *)_work;
	vma_op_request_t *req = work->msg;
	struct task_struct *tsk = __get_task_struct(req->origin_pid);
	struct list_head *entry = &((struct work_struct *)work)->entry;
	struct remote_context *rc;

	if (!tsk) goto out_free;

	rc = get_task_remote(tsk);

	INIT_LIST_HEAD(entry);
	spin_lock(&rc->vma_works_lock);
	list_add(entry, &rc->vma_works);
	spin_unlock(&rc->vma_works_lock);

	complete(&rc->vma_works_ready);

	put_task_remote(tsk);
	put_task_struct(tsk);

	return;

out_free:
	WARN_ON("No target task");
	pcn_kmsg_free_msg(req);
	kfree(work);
}

static void process_vma_op_response(struct work_struct *_work)
{
	struct pcn_kmsg_work *work = (struct pcn_kmsg_work *)_work;
	vma_op_response_t *res = work->msg;
	struct wait_station *ws = wait_station(res->remote_ws);

	ws->private = res;
	smp_mb();
	complete(&ws->pendings);

	kfree(work);
}



/**
 * Response for remote VMA request and handling the response
 */

struct vma_info {
	struct list_head list;
	unsigned long addr;
	bool mapped;
	atomic_t pendings;
	wait_queue_head_t pendings_wait;
	int ret;

	remote_vma_response_t *response;
};


static struct vma_info *__lookup_pending_vma_request(struct remote_context *rc, unsigned long addr)
{
	struct vma_info *vi;

	list_for_each_entry(vi, &rc->vmas, list) {
		if (vi->addr == addr) return vi;
	}

	return NULL;
}


static void process_remote_vma_response(struct work_struct *_work)
{
	struct pcn_kmsg_work *w = (struct pcn_kmsg_work *)_work;
	remote_vma_response_t *res = w->msg;
	struct task_struct *tsk;
	unsigned long flags;
	struct vma_info *vi;
	struct remote_context *rc;

	kfree(w);

	tsk = __get_task_struct(res->remote_pid);
	if (!tsk) {
		__WARN();
		goto out_free;
	}
	rc = get_task_remote(tsk);

	spin_lock_irqsave(&rc->vmas_lock, flags);
	vi = __lookup_pending_vma_request(rc, res->addr);
	spin_unlock_irqrestore(&rc->vmas_lock, flags);
	put_task_remote(tsk);
	put_task_struct(tsk);

	if (!vi) {
		__WARN();
		goto out_free;
	}

	VSPRINTK("  [%d] <-- %d, %d pended\n", tsk->pid,
			res->result, atomic_read(&vi->pendings));
	vi->response = res;
	wake_up(&vi->pendings_wait);
	return;

out_free:
	pcn_kmsg_free_msg(res);
	return;
}


static void response_remote_vma(int remote_nid, int remote_pid, remote_vma_response_t *res)
{
	res->header.type = PCN_KMSG_TYPE_REMOTE_VMA_RESPONSE;
	res->header.prio = PCN_KMSG_PRIO_NORMAL;

	res->remote_pid = remote_pid;

	pcn_kmsg_send(remote_nid, res, sizeof(*res));
}


/**
 * Request for remote vma and handling the request
 */

static void process_remote_vma_request(struct work_struct *work)
{
	struct pcn_kmsg_work *w = (struct pcn_kmsg_work *)work;
	remote_vma_request_t *req = w->msg;
	remote_vma_response_t *res = NULL;
	struct task_struct *tsk;
	struct mm_struct *mm;
	struct vm_area_struct *vma;
	unsigned long addr = req->addr;

	might_sleep();

	while (!res) {
		res = kmalloc(sizeof(*res), GFP_KERNEL);
	}
	res->addr = addr;

	tsk = __get_task_struct(req->origin_pid);
	if (!tsk) {
		printk("remote_vma:: process does not exist %d\n", req->origin_pid);
		res->result = -ESRCH;
		goto out_free;
	}

	mm = get_task_mm(tsk);

	BUG_ON(!process_is_distributed(tsk));

	/**
	 * This processing is insipired from the VMA fault handling at the
	 * beginning of __do_page_fault.
	 */
	down_read(&mm->mmap_sem);

	/* Fill-in the vma info */
	vma = find_vma(mm, addr);
	if (unlikely(!vma)) {
		printk("remote_vma: does not exist for %lx\n", addr);
		res->result = -ENOENT;
		goto out_up;
	}
	if (likely(vma->vm_start <= addr)) {
		goto good;
	}
	if (unlikely(!(vma->vm_flags & VM_GROWSDOWN))) {
		printk("remote_vma: does not really exist for %lx\n", addr);
		res->result = -ENOENT;
		goto out_up;
	}

good:
	res->vm_start = vma->vm_start;
	res->vm_end = vma->vm_end;
	res->vm_flags = vma->vm_flags;
	res->vm_pgoff = vma->vm_pgoff;

	get_file_path(vma->vm_file, res->vm_file_path, sizeof(res->vm_file_path));
	res->result = 0;

	set_bit(req->remote_nid, vma->vm_owners);

out_up:
	up_read(&mm->mmap_sem);
	mmput(mm);
	put_task_struct(tsk);

	if (res->result == 0) {
		VSPRINTK("remote_vma: %lx -- %lx %lx\n",
				res->vm_start, res->vm_end, res->vm_flags);
		if (!remote_vma_anon(res)) {
			VSPRINTK("remote_vma: %lx %s\n", res->vm_pgoff, res->vm_file_path);
		}
	}

out_free:
	response_remote_vma(req->remote_nid, req->remote_pid, res);

	pcn_kmsg_free_msg(req);
	kfree(w);
	kfree(res);

	return;
}


static struct vma_info *__alloc_remote_vma_request(struct task_struct *tsk, unsigned long addr, remote_vma_request_t **preq)
{
	struct vma_info *vi = kmalloc(sizeof(*vi), GFP_KERNEL);
	remote_vma_request_t *req = kmalloc(sizeof(*req), GFP_KERNEL);

	BUG_ON(!vi || !req);

	/* vma_info */
	INIT_LIST_HEAD(&vi->list);
	vi->addr = addr;
	vi->mapped = false;
	atomic_set(&vi->pendings, 0);
	init_waitqueue_head(&vi->pendings_wait);

	/* req */
	req->header.type = PCN_KMSG_TYPE_REMOTE_VMA_REQUEST;
	req->header.prio = PCN_KMSG_PRIO_NORMAL;

	req->origin_pid = tsk->origin_pid;
	req->remote_nid = my_nid;
	req->remote_pid = tsk->pid;
	req->addr = addr;

	*preq = req;

	return vi;
}


int __map_remote_vma(struct task_struct *tsk, struct vma_info *vi)
{
	struct mm_struct *mm = tsk->mm;
	struct vm_area_struct *vma;
	unsigned long prot;
	unsigned flags = MAP_FIXED;
	struct file *f = NULL;
	unsigned long err = 0;
	int ret = 0;
	unsigned long addr = vi->response->addr;
	remote_vma_response_t *res = vi->response;

	if (res->result) {
		down_read(&mm->mmap_sem);
		return res->result;
	}

	down_write(&mm->mmap_sem);
	vma = find_vma(mm, addr);
	if (vma && vma->vm_start <= addr) {
		/* someone already do things for me. */
		goto out;
	}

	if (remote_vma_anon(res)) {
		flags |= MAP_ANONYMOUS;
	} else {
		f = filp_open(res->vm_file_path, O_RDONLY | O_LARGEFILE, 0);
		if (IS_ERR(f)) {
			printk(KERN_ERR"%s: cannot find backing file %s\n",__func__,
				res->vm_file_path);
			ret = -EIO;
			goto out;
		}
	}

	prot  = ((res->vm_flags & VM_READ) ? PROT_READ : 0)
			| ((res->vm_flags & VM_WRITE) ? PROT_WRITE : 0)
			| ((res->vm_flags & VM_EXEC) ? PROT_EXEC : 0);

	flags = flags
		   | ((res->vm_flags & VM_DENYWRITE) ? MAP_DENYWRITE : 0)
		   | ((res->vm_flags & VM_SHARED) ? MAP_SHARED : MAP_PRIVATE)
		   | ((res->vm_flags & VM_GROWSDOWN) ? MAP_GROWSDOWN : 0);

	err = map_difference(mm, f, res->vm_start, res->vm_end,
				prot, flags, res->vm_pgoff);

	if (f) filp_close(f, NULL);

	vma = find_vma(mm, addr);
	BUG_ON(!vma || vma->vm_start > addr);

out:
	downgrade_write(&mm->mmap_sem);
	return ret;
}


int vma_server_fetch_vma(struct task_struct *tsk, unsigned long address)
{
	struct vma_info *vi;
	unsigned long flags;
	DEFINE_WAIT(wait);
	int ret = 0;
	unsigned long addr = address & PAGE_MASK;
	remote_vma_request_t *req = NULL;
	struct remote_context *rc = get_task_remote(tsk);
	bool wakeup = false;

	might_sleep();

	VSPRINTK("\n");
	VSPRINTK("## VMAFAULT [%d] %lx %lx\n",
			current->pid, address, instruction_pointer(current_pt_regs()));

	spin_lock_irqsave(&rc->vmas_lock, flags);
	vi = __lookup_pending_vma_request(rc, addr);
	if (!vi) {
		struct vma_info *v;
		spin_unlock_irqrestore(&rc->vmas_lock, flags);

		vi = __alloc_remote_vma_request(tsk, addr, &req);

		spin_lock_irqsave(&rc->vmas_lock, flags);
		v = __lookup_pending_vma_request(rc, addr);
		if (!v) {
			VSPRINTK("  [%d] %lx from %d at %d\n",
					current->pid, addr, tsk->origin_pid, tsk->origin_nid);
			smp_mb();
			list_add(&vi->list, &rc->vmas);
		} else {
			printk("  [%d] %lx already pended\n",
					current->pid, addr);
			kfree(vi);
			vi = v;
			kfree(req);
			req = NULL;
		}
	} else {
		VSPRINTK("  [%d] %lx already pended\n", current->pid, addr);
	}
	atomic_inc(&vi->pendings);
	prepare_to_wait(&vi->pendings_wait, &wait, TASK_UNINTERRUPTIBLE);
	spin_unlock_irqrestore(&rc->vmas_lock, flags);

	if (req) {
		pcn_kmsg_send(tsk->origin_nid, req, sizeof(*req));
		kfree(req);
	}

	up_read(&tsk->mm->mmap_sem);
	io_schedule();

	/**
	 * Now vi->response should points to the result
	 * Also, mm_mmap_sem should be properly set when return
	 */

	finish_wait(&vi->pendings_wait, &wait);

	if (!vi->mapped) {
		vi->ret = __map_remote_vma(tsk, vi);
		vi->mapped = true;
	} else {
		down_read(&tsk->mm->mmap_sem);
	}
	ret = vi->ret;

	VSPRINTK("  [%d] %lx resume %d\n", current->pid, addr, ret);

	spin_lock_irqsave(&rc->vmas_lock, flags);
	if (atomic_dec_return(&vi->pendings)) {
		wakeup = true;
	} else {
		list_del(&vi->list);
		pcn_kmsg_free_msg(vi->response);
		kfree(vi);
	}
	spin_unlock_irqrestore(&rc->vmas_lock, flags);
	put_task_remote(tsk);
	smp_mb();

	if (wakeup) wake_up(&vi->pendings_wait);

	return ret;
}

DEFINE_KMSG_ORDERED_WQ_HANDLER(vma_lock);
DEFINE_KMSG_WQ_HANDLER(vma_op);

DEFINE_KMSG_WQ_HANDLER(remote_vma_request);
DEFINE_KMSG_WQ_HANDLER(remote_vma_response);
DEFINE_KMSG_WQ_HANDLER(vma_op_request);
DEFINE_KMSG_NONBLOCK_WQ_HANDLER(vma_op_response);

int vma_server_init(void)
{
	pcn_kmsg_register_callback(PCN_KMSG_TYPE_VMA_ACK, handle_vma_ack);
	REGISTER_KMSG_WQ_HANDLER(PCN_KMSG_TYPE_VMA_LOCK, vma_lock);
	REGISTER_KMSG_WQ_HANDLER(PCN_KMSG_TYPE_VMA_OP, vma_op);

	REGISTER_KMSG_WQ_HANDLER(
			PCN_KMSG_TYPE_REMOTE_VMA_REQUEST, remote_vma_request);
	REGISTER_KMSG_WQ_HANDLER(
			PCN_KMSG_TYPE_REMOTE_VMA_RESPONSE, remote_vma_response);

	REGISTER_KMSG_WQ_HANDLER(PCN_KMSG_TYPE_VMA_OP_REQUEST, vma_op_request);
	REGISTER_KMSG_WQ_HANDLER(PCN_KMSG_TYPE_VMA_OP_RESPONSE, vma_op_response);

	return 0;
}
