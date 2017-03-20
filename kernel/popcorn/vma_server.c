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

#include <popcorn/cpuinfo.h>
#include <popcorn/bundle.h>
#include <popcorn/debug.h>
#include <popcorn/process_server.h>

#include "types.h"
#include "vma_server.h"


static struct workqueue_struct *vma_op_wq;


///////////////////////////////////////////////////////////////////////////////
// Heterogeneous binary support
///////////////////////////////////////////////////////////////////////////////
/* ajith - for file offset fetch. copied from fs/binfmt_elf.c*/

#if ELF_EXEC_PAGESIZE > PAGE_SIZE
#define ELF_MIN_ALIGN   ELF_EXEC_PAGESIZE
#else
#define ELF_MIN_ALIGN   PAGE_SIZE
#endif

/* Ajith - adding file offset parsing */
static unsigned long get_file_offset(struct file *file, int start_addr)
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
	retval = kernel_read(file, elf_ex.e_phoff,
			     (char *)elf_eppnt, size);
	if (retval != size) {
		printk("%s: ERROR: during kernel read of ELF file\n", __func__);
		retval = -1;
		goto out;
	}
	for (i = 0; i < elf_ex.e_phnum; i++, elf_eppnt++) {
		if (elf_eppnt->p_type == PT_LOAD) {

			printk("%s: Page offset for 0x%x 0x%lx 0x%lx\n", __func__,
					start_addr,
					(unsigned long)elf_eppnt->p_vaddr,
					(unsigned long)elf_eppnt->p_memsz);

			if ((start_addr >= elf_eppnt->p_vaddr)&&
					(start_addr <= (elf_eppnt->p_vaddr+elf_eppnt->p_memsz))) {
				printk("%s: Finding page offset for 0x%x 0x%lx 0x%lx\n",
					__func__, start_addr,
					(unsigned long)elf_eppnt->p_vaddr,
					(unsigned long)elf_eppnt->p_memsz);
				retval = elf_eppnt->p_offset -
						(elf_eppnt->p_vaddr & (ELF_MIN_ALIGN-1));
				goto out;
			}
			/*
			if ((elf_eppnt->p_flags & PF_R) && (elf_eppnt->p_flags & PF_X)) {
				printk("Coming to executable program load section\n");
				retval = elf_eppnt->p_offset -
						(elf_eppnt->p_vaddr & (ELF_MIN_ALIGN-1));
				goto out;
			}
			*/
		}
	}

out:
	if (elf_eppnt_start != NULL)
		kfree(elf_eppnt_start);

	return retval >> PAGE_SHIFT;
}


///////////////////////////////////////////////////////////////////////////////
// Legacy codes. Remove them quickly
///////////////////////////////////////////////////////////////////////////////
typedef struct mapping_answers_2_kernels {
	struct mapping_answers_2_kernels* next;
	struct mapping_answers_2_kernels* prev;

	int tgroup_home_cpu;
	int tgroup_home_id;
	unsigned long address;
	int vma_present;
	unsigned long vaddr_start;
	unsigned long vaddr_size;
	unsigned long pgoff;
	char path[512];
	pgprot_t prot;
	unsigned long vm_flags;
	int is_write;
	int is_fetch;
	int owner;
	int address_present;
	long last_write;
	int owners [MAX_KERNEL_IDS];
	data_response_for_2_kernels_t* data;
	int arrived_response;
	struct task_struct* waiting;
	int futex_owner;
} mapping_answers_for_2_kernels_t;


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


static inline void add_vma_ack_entry(vma_op_answers_t* entry)
{
	return;
}

static inline vma_op_answers_t* find_vma_ack_entry(int cpu, int id)
{
	return NULL;
}

static inline void remove_vma_ack_entry(vma_op_answers_t* entry)
{
	return;
}

static vma_op_answers_t * vma_op_answer_alloc(struct task_struct * task, int index)
{
	vma_op_answers_t *acks = kzalloc(sizeof(*acks), GFP_ATOMIC);
	if (!acks) return NULL;

	acks->origin_nid = task->origin_nid;
	acks->origin_pid = task->origin_pid;
	acks->vma_operation_index = index;
	acks->waiting = task;
	acks->responses = 0;
	acks->expected_responses = 0;
	spin_lock_init(&(acks->lock));
	add_vma_ack_entry(acks);

	return acks;
}



/*****************************************************************************/
/* Messaging related stuff                                                   */
/*****************************************************************************/

// TODO this have to me moved somewhere else but made more generic (!!!)
static inline int vma_send_long_all( memory_t * entry, void * message, int size,
		struct task_struct * task, int max_distr_vma_op)
{
	int i, acks =0;
	_remote_cpu_info_list_t *objPtr, *tmpPtr;

	// the list does not include the current processor group descirptor (TODO)
	list_for_each_entry_safe(objPtr, tmpPtr, &rlist_head, cpu_list_member) {
		int error;
		i = objPtr->_data._processor;

		if ( task && (task->mm->distr_vma_op_counter > max_distr_vma_op)
				&& (i == entry->message_push_operation->from_cpu))
			continue;
		error = pcn_kmsg_send_long(i, message, size);
		if (error != -1)
			acks++;
	}
	return acks;
}

static vma_lock_t * vma_lock_alloc(struct task_struct * task, int from_cpu, int index)
{
	vma_lock_t* lock_message = kmalloc(sizeof(vma_lock_t), GFP_ATOMIC);
	if (!lock_message)
		return NULL;

	lock_message->header.type = PCN_KMSG_TYPE_PROC_SRV_VMA_LOCK;
	lock_message->header.prio = PCN_KMSG_PRIO_NORMAL;

	lock_message->origin_nid = task->origin_nid;
	lock_message->origin_pid = task->origin_pid;
	lock_message->from_cpu = from_cpu;
	lock_message->vma_operation_index = index;
	return lock_message;
}

static vma_operation_t * vma_operation_alloc(struct task_struct * task, int op_id,
		unsigned long addr, unsigned long new_addr, int len, int new_len,
		unsigned long prot, unsigned long flags, int index)
{
	vma_operation_t* operation = kmalloc(sizeof(vma_operation_t), GFP_ATOMIC);
	if (!operation)
		return NULL;

	operation->header.type = PCN_KMSG_TYPE_PROC_SRV_VMA_OP;
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
	operation->from_cpu = my_nid;
	return operation;
}

#if 0
//TODO --- questo stesso codice ce due volte in process_vma_operation
	vma_operation_copy
	mm->thread_op = memory->is_vma_worker;

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
	wake_up_process(memory->is_vma_worker);
	PSPRINTK("%s,SERVER: woke up the main\n", __func__);

	while (memory->operation != VMA_OP_NOP) {
		set_task_state(current, TASK_UNINTERRUPTIBLE);
		if (memory->operation != VMA_OP_NOP) {
			schedule();
		}
		set_task_state(current, TASK_RUNNING);
	}
	PSPRINTK("%s,SERVER: woke up the main1\n", __func__);

	down_write(&mm->mmap_sem);
	PSPRINTK("%s,SERVER: woke up the main2\n", __func__);


/* THIS CAN BE DESTROY EQUIVALENT OF THE ABOVE
	unsigned long flags;
	spin_lock_irqsave(&(acks->lock), flags);
	remove_vma_ack_entry(acks);
	spin_unlock_irqrestore(&(acks->lock), flags);

	// TODO
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
	wake_up_process(memory->is_vma_worker);
	PSPRINTK("%s,SERVER: woke up the main\n", __func__);

	while (memory->operation != VMA_OP_NOP) {
		set_task_state(current, TASK_UNINTERRUPTIBLE);
		if (memory->operation != VMA_OP_NOP) {
			schedule();
		}
		set_task_state(current, TASK_RUNNING);
	}
	PSPRINTK("%s,SERVER: woke up the main1\n", __func__);*/
#endif


///////////////////////////////////////////////////////////////////////////////
// vma operations
///////////////////////////////////////////////////////////////////////////////

static unsigned long map_difference(struct mm_struct *mm, struct file *file,
		unsigned long start, unsigned long end,
		unsigned long prot, unsigned long flags,
		unsigned long pgoff)
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
			VSPRINTK("map0 [%d]: %lx -- %lx @ %lx\n",
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
			VSPRINTK("map1 [%d]: %lx -- %lx, %lx\n",
					current->pid, start, vma->vm_start, pgoff);
			if (error != start) {
				ret = VM_FAULT_SIGBUS;;
			}
			break;
		} else if (start <= vma->vm_start && vma->vm_end <= end) {
			// the VMA is fully within the region of interest

			error = do_mmap_pgoff(file, start, vma->vm_start - start,
					prot, flags, pgoff, &populate);
			VSPRINTK("map2 [%d]: %lx -- %lx, %lx\n",
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

/* which locks are held when entering here?!
 * mm->mm_sem is taken in down_read
 * ptl is locked (on the pte)
 *
 * ptl is from pte_offset_map_lock (this is a lock on the page table entry somewhere)
 */
int vma_server_do_mapping_for_distributed_process(
		struct mapping_answers_for_2_kernels* _fetching_page,
		struct task_struct *tsk, struct mm_struct* mm,
		unsigned long address, spinlock_t* ptl)
{

	struct vm_area_struct* vma;
	unsigned long prot = 0;
	unsigned long err, ret;
	mapping_answers_for_2_kernels_t *fetching_page =
		(mapping_answers_for_2_kernels_t *)_fetching_page;

	prot |= (fetching_page->vm_flags & VM_READ) ? PROT_READ : 0;
	prot |= (fetching_page->vm_flags & VM_WRITE) ? PROT_WRITE : 0;
	prot |= (fetching_page->vm_flags & VM_EXEC) ? PROT_EXEC : 0;

	if (fetching_page->vma_present != 1) {
		// NOTE this case is not handled and returns 0! (vma_present == 0)
		return 0;
	}

	if (fetching_page->path[0] == '\0') { // anonymous page mapping
		vma = find_vma(mm, address);
		if (!vma || address >= vma->vm_end || address < vma->vm_start) {
			vma = NULL;
		}

		if (!vma
			|| (vma->vm_start != fetching_page->vaddr_start)
			|| (vma->vm_end != (fetching_page->vaddr_start + fetching_page->vaddr_size))) {

			spin_unlock(ptl);
			/*PTE UNLOCKED*/

			/* Note: during a page fault the distribute lock is held in read =>
			 * distributed vma operations cannot happen in the same time
			 */
			up_read(&mm->mmap_sem);
			down_write(&mm->mmap_sem);

			/* when I release the down write on mmap_sem, another thread of my process
			 * could install the same vma that I am trying to install
			 * (only fetch of same addresses are prevent, not fetch of different addresses on the same vma)
			 * take the newest vma.
			 * */
			vma = find_vma(mm, address);
			if (!vma || address >= vma->vm_end || address < vma->vm_start) {
				vma = NULL;
			}

			/* All vma operations are distributed, except for mmap =>
			 * When I receive a vma, the only difference can be on the size (start, end) of the vma.
			 */
			if ( !vma
				|| (vma->vm_start != fetching_page->vaddr_start)
				|| (vma->vm_end != (fetching_page->vaddr_start + fetching_page->vaddr_size)) ) {
				PSPRINTK("Mapping anonymous vma start %lx end %lx\n",
					fetching_page->vaddr_start,
					(fetching_page->vaddr_start + fetching_page->vaddr_size));

				/*Note:
				 * This mapping is caused because when a thread migrates it does not have any vma
				 * so during fetch vma can be pushed.
				 * This mapping has the precedence over "normal" vma operations because is a page fault
				 * */
				if (tsk->mm->distribute_unmap == 0)
					printk(KERN_ALERT"%s: ERROR: anon value was already 0, check who is the older.\n", __func__);
				tsk->mm->distribute_unmap = 0;

				/*map_difference should map in such a way that no unmap operations (the only nested operation that mmap can call) are nested called.
				 * This is important both to not unmap pages that should not be unmapped
				 * but also because otherwise the vma protocol will deadlock!
				 */
				err = map_difference(tsk->mm, NULL, fetching_page->vaddr_start,
						 fetching_page->vaddr_start + fetching_page->vaddr_size, prot,
						 MAP_FIXED | MAP_ANONYMOUS
						 | ((fetching_page->vm_flags & VM_SHARED) ? MAP_SHARED : MAP_PRIVATE)
						 | ((fetching_page->vm_flags & VM_HUGETLB) ? MAP_HUGETLB : 0)
						 | ((fetching_page->vm_flags & VM_GROWSDOWN) ? MAP_GROWSDOWN : 0), 0);

				if (tsk->mm->distribute_unmap == 1)
					printk(KERN_ALERT"%s: ERROR: anon value was already 1, check who is the older.\n", __func__);
				tsk->mm->distribute_unmap = 1;

				if (err != fetching_page->vaddr_start) {
					up_write(&mm->mmap_sem);
					down_read(&mm->mmap_sem);
					spin_lock(ptl);
					/*PTE LOCKED*/
					printk(
						"%s: ERROR: error mapping anonymous vma while fetching address %lx\n",
						__func__, address);
					ret = VM_FAULT_VMA;
					return ret;
				}
			}

			up_write(&mm->mmap_sem);
			down_read(&mm->mmap_sem);
			spin_lock(ptl);
			/*PTE LOCKED*/
		}
	} else { //not anonymous page
		vma = find_vma(mm, address);
		if (!vma || address >= vma->vm_end || address < vma->vm_start) {
			vma = NULL;
		}

		if (!vma
			|| (vma->vm_start != fetching_page->vaddr_start)
			|| (vma->vm_end != (fetching_page->vaddr_start + fetching_page->vaddr_size))) {
			struct file* f;

			spin_unlock(ptl);
			/*PTE UNLOCKED*/

			up_read(&mm->mmap_sem);

			f = filp_open(fetching_page->path, O_RDONLY | O_LARGEFILE, 0);

			if (IS_ERR(f)) {
				down_read(&mm->mmap_sem);
				spin_lock(ptl);
				/*PTE LOCKED*/
				printk("%s: ERROR: error while opening file %s\n",
						__func__, fetching_page->path);
				ret = VM_FAULT_VMA;
				return ret;
			}

			down_write(&mm->mmap_sem);

			//check if other threads already installed the vma
			vma = find_vma(mm, address);
			if (!vma || address >= vma->vm_end || address < vma->vm_start) {
				vma = NULL;
			}

			if (!vma
				|| (vma->vm_start != fetching_page->vaddr_start)
				|| (vma->vm_end != (fetching_page->vaddr_start + fetching_page->vaddr_size)) ) {

				PSPRINTK("Mapping file vma start %lx end %lx\n",
					fetching_page->vaddr_start,
					(fetching_page->vaddr_start + fetching_page->vaddr_size));

				/*Note:
				 * This mapping is caused because when a thread migrates it does not have any vma
				 * so during fetch vma can be pushed.
				 * This mapping has the precedence over "normal" vma operations because is a page fault
				 * */
				if (tsk->mm->distribute_unmap == 0)
					printk(KERN_ALERT"%s: ERROR: file backed value was already 0, check who is the older.\n", __func__);
				tsk->mm->distribute_unmap = 0;

				PSPRINTK("%s:%d page offset = %lu 0x%p\n", __func__,__LINE__,
						fetching_page->pgoff, mm->exe_file);
				fetching_page->pgoff = get_file_offset(mm->exe_file, fetching_page->vaddr_start);
				PSPRINTK("%s:%d page offset = %lu\n", __func__, __LINE__,
						fetching_page->pgoff);

				/*map_difference should map in such a way that no unmap operations (the only nested operation that mmap can call) are nested called.
				 * This is important both to not unmap pages that should not be unmapped
				 * but also because otherwise the vma protocol will deadlock!
				 */
				err = map_difference(tsk->mm, f, fetching_page->vaddr_start,
						   fetching_page->vaddr_start + fetching_page->vaddr_size, prot,
						   MAP_FIXED
						   | ((fetching_page->vm_flags & VM_DENYWRITE) ? MAP_DENYWRITE : 0)
						   /* Ported to Linux 3.12
						  | ((fetching_page->vm_flags & VM_EXECUTABLE) ? MAP_EXECUTABLE : 0) */
						   | ((fetching_page->vm_flags & VM_SHARED) ? MAP_SHARED : MAP_PRIVATE)
						   | ((fetching_page->vm_flags & VM_HUGETLB) ? MAP_HUGETLB : 0),
						   fetching_page->pgoff << PAGE_SHIFT);
				if (tsk->mm->distribute_unmap == 1)
					printk(KERN_ALERT"%s: ERROR: file backed value was already 1, check who is the older.\n", __func__);
				tsk->mm->distribute_unmap = 1;

				PSPRINTK("Map difference ended\n");
				if (err != fetching_page->vaddr_start) {
					up_write(&mm->mmap_sem);
					down_read(&mm->mmap_sem);
					spin_lock(ptl);
					/*PTE LOCKED*/
					printk("%s: ERROR: error mapping file vma while fetching address %lx\n",
						__func__, address);
					ret = VM_FAULT_VMA;
					return ret;
				}
			}


			up_write(&mm->mmap_sem);
			PSPRINTK("releasing lock write\n");
			filp_close(f, NULL);

			down_read(&mm->mmap_sem);
			PSPRINTK("lock read taken\n");
			spin_lock(ptl);
			/*PTE LOCKED*/
		}
	} //end not anonymous case
	return 0;
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
	int fake;
};

enum {
	EXIT_FLUSHING,
};

DECLARE_WAIT_QUEUE_HEAD(request_distributed_vma_op);

static void vma_server_process_vma_op(struct work_struct* work)
{
	struct vma_op_work* vma_work = (struct vma_op_work*) work;
	vma_operation_t* operation = vma_work->operation;
	memory_t* memory = vma_work->memory;
	struct mm_struct* mm = NULL;

	//to coordinate with death of process
	if (vma_work->fake == 1) {
		unsigned long flags;
		memory->arrived_op = 1;
		lock_task_sighand(memory->helper, &flags);
		memory->helper->distributed_exit = EXIT_FLUSHING;
		unlock_task_sighand(memory->helper, &flags);
		wake_up_process(memory->helper);
		kfree(work);
		return;
	}

	if (!memory) {
		printk(KERN_ERR"%s: ERROR: vma_work->memory is 0x%lx\n",
				__func__, (unsigned long)memory);
		return;
	}
	mm = memory->mm;
	if (!mm) {
		printk(KERN_ERR"%s: ERROR: vma_memory->mm is 0x%lx\n",
				__func__, (unsigned long)mm);
		return;
	}
	PSPRINTK("Received vma operation from cpu %d for origin_nid %i "
			"origin_pid %i operation %i\n",
			operation->header.from_cpu, operation->origin_nid,
			operation->origin_pid, operation->operation);

	down_write(&mm->mmap_sem);
	if (my_nid == operation->origin_nid) { //SERVER
		//if another operation is on going, it will be serialized after.
#if 0
		while (mm->distr_vma_op_counter > 0) {
			printk("%s, A distributed operation already started, going to sleep\n", __func__);
			up_write(&mm->mmap_sem);
			DEFINE_WAIT(wait);
			prepare_to_wait(&request_distributed_vma_op, &wait,
					TASK_UNINTERRUPTIBLE);
			if (mm->distr_vma_op_counter > 0) {
				schedule();
			}
			finish_wait(&request_distributed_vma_op, &wait);
			down_write(&mm->mmap_sem);
		}

		//here the value is either 0 or negative ...how it can be negative?
		if (mm->distr_vma_op_counter != 0 || mm->was_not_pushed != 0) {
			up_write(&mm->mmap_sem);
			printk("ERROR: handling a new vma operation but mm->distr_vma_op_counter is %i and mm->was_not_pushed is %i\n",
			       mm->distr_vma_op_counter, mm->was_not_pushed);
			pcn_kmsg_free_msg(operation);
			kfree(work);
			return ;
		}
		PSPRINTK("%s,SERVER: Starting operation %i for cpu %i\n", __func__, operation->operation, operation->header.from_cpu);

		mm->distr_vma_op_counter++;
		//the main kernel thread will execute the local operation
#else
		//original code has concurrency errors -- this is similar to the client code (client code not in Marina's terms)
		if (mm->distr_vma_op_counter < 0)
			printk(KERN_ALERT"%s: ERROR: distr_vma_op_counter is %d",
					__func__, mm->distr_vma_op_counter);
		// ONLY ONE CAN BE IN DISTRIBUTED OPERATION AT THE TIME ... WHAT ABOUT NESTED OPERATIONS?
		while (atomic_cmpxchg((atomic_t*)&(mm->distr_vma_op_counter), 0, 1) != 0) { //success is indicated by comparing RETURN with OLD (arch/x86/include/asm/cmpxchg.h)
			DEFINE_WAIT(wait);
			up_write(&mm->mmap_sem);

			prepare_to_wait(&request_distributed_vma_op, &wait, TASK_UNINTERRUPTIBLE);
			if (mm->distr_vma_op_counter > 0) {
				printk("%s: LOCK: Somebody already started a distributed "
						"operation (mm->thread_op->pid %d). "
						"I am pid worker %d and I am going to sleep.\n",
						__func__, mm->thread_op->pid, current->pid);
				schedule();
			}
			finish_wait(&request_distributed_vma_op, &wait);

			down_write(&mm->mmap_sem);
		}
		// here the distr_vma_op_counter is ours so let's check only the was_not_pushed
		if (mm->was_not_pushed != 0) {
			up_write(&mm->mmap_sem);
			current->mm->distr_vma_op_counter--;
			printk("%s: ERROR: handling a new vma operation but "
					"mm->distr_vma_op_counter is %i "
					"and mm->was_not_pushed is %i\n",
			       __func__, mm->distr_vma_op_counter, mm->was_not_pushed);
			pcn_kmsg_free_msg(operation);
			kfree(work);
			return ;
		}
#endif
		while (memory->helper==NULL)
			schedule();

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
		PSPRINTK("%s,SERVER: woke up the main\n", __func__);

		while (memory->operation != VMA_OP_NOP) {
			set_task_state(current, TASK_UNINTERRUPTIBLE);
			if (memory->operation != VMA_OP_NOP) {
				schedule();
			}
			set_task_state(current, TASK_RUNNING);
		}
		PSPRINTK("%s,SERVER: woke up the main1\n", __func__);

		down_write(&mm->mmap_sem);
		PSPRINTK("%s,SERVER: woke up the main2\n", __func__);

		mm->distr_vma_op_counter--;
		if (mm->distr_vma_op_counter != 0)
			printk(	"%s: ERROR: exiting from distributed operation "
					"but mm->distr_vma_op_counter is %i\n",
				__func__, mm->distr_vma_op_counter);
		if (operation->operation == VMA_OP_MAP
		    || operation->operation == VMA_OP_BRK) {
			mm->was_not_pushed--;
			if (mm->was_not_pushed != 0)
				printk("%s: ERROR: exiting from distributed operation "
						"but mm->was_not_pushed is %i\n",
				       __func__, mm->distr_vma_op_counter);
		}

		mm->thread_op = NULL;

		PSPRINTK("%s,SERVER: woke up the main3\n", __func__);
		up_write(&mm->mmap_sem);
		PSPRINTK("%s,SERVER: woke up the main4\n", __func__);

		wake_up(&request_distributed_vma_op);

		pcn_kmsg_free_msg(operation);
		kfree(work);
		PSPRINTK("SERVER: vma_operation_index is %d\n",mm->vma_operation_index);
		PSPRINTK("%s, SERVER: end requested operation\n", __func__);
		return ;
	} else {
		struct task_struct *prev;
		PSPRINTK("%s, CLIENT: Starting operation %i of index %i\n ", __func__,
				operation->operation, operation->vma_operation_index);
		//CLIENT

		//NOTE: the current->mm->distribute_sem is already held

		//MMAP and BRK are not pushed in the system
		//if I receive one of them I must have initiate it
		if (operation->operation == VMA_OP_MAP
		    || operation->operation == VMA_OP_BRK) {

			if (memory->my_lock != 1) {
				printk("%s: ERROR: wrong distributed lock aquisition\n", __func__);
				up_write(&mm->mmap_sem);
				pcn_kmsg_free_msg(operation);
				kfree(work);
				return ;
			}

			if (operation->from_cpu != my_nid) {
				printk("%s: ERROR: the server pushed me an operation %i of cpu %i\n",
				       __func__, operation->operation, operation->from_cpu);
				up_write(&mm->mmap_sem);
				pcn_kmsg_free_msg(operation);
				kfree(work);
				return ;
			}

			if (memory->waiting_for_op == NULL) {
				printk(	"%s: ERROR:received a push operation started by me but nobody is waiting\n", __func__);
				up_write(&mm->mmap_sem);
				pcn_kmsg_free_msg(operation);
				kfree(work);
				return ;
			}

			memory->addr = operation->addr;
			memory->arrived_op = 1;
			//mm->vma_operation_index = operation->vma_operation_index;
			PSPRINTK("CLIENT: vma_operation_index is %d\n",mm->vma_operation_index);
			PSPRINTK("%s, CLIENT: Operation %i started by a local thread pid %d\n ", __func__,operation->operation,memory->waiting_for_op->pid);
			up_write(&mm->mmap_sem);

			wake_up_process(memory->waiting_for_op);

			pcn_kmsg_free_msg(operation);
			kfree(work);
			return ;
		}

		//I could have started the operation...check!
		if (operation->from_cpu == my_nid) {
			if (memory->my_lock != 1) {
				printk("%s: ERROR: wrong distributed lock aquisition\n", __func__);
				up_write(&mm->mmap_sem);
				pcn_kmsg_free_msg(operation);
				kfree(work);
				return ;
			}

			if (memory->waiting_for_op == NULL) {
				printk(
					"%s: ERROR:received a push operation started by me but nobody is waiting\n", __func__);
				up_write(&mm->mmap_sem);
				pcn_kmsg_free_msg(operation);
				kfree(work);
				return ;
			}
			if (operation->operation == VMA_OP_REMAP)
				memory->addr = operation->new_addr;

			memory->arrived_op = 1;
			//mm->vma_operation_index = operation->vma_operation_index;
			PSPRINTK("%s, CLIENT: Operation %i started by a local thread pid %d index %d\n ",
					__func__,operation->operation,memory->waiting_for_op->pid, mm->vma_operation_index);
			up_write(&mm->mmap_sem);

			wake_up_process(memory->waiting_for_op);
			pcn_kmsg_free_msg(operation);
			kfree(work);
			return ;
		}

		PSPRINTK("%s, CLIENT Pushed operation started by somebody else\n", __func__);
		if (operation->addr < 0) {
			printk("%s: WARN: server sent me and error\n", __func__);
			pcn_kmsg_free_msg(operation);
			kfree(work);
			return ;
		}

		mm->distr_vma_op_counter++;
		prev = mm->thread_op;

		while (memory->helper==NULL) // waiting for main to be allocated (?)
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

		PSPRINTK("%s,CLIENT: woke up the main5\n", __func__);

		while (memory->operation != VMA_OP_NOP) {
			set_task_state(current, TASK_UNINTERRUPTIBLE);
			if (memory->operation != VMA_OP_NOP) {
				schedule();
			}
			set_task_state(current, TASK_RUNNING);
		}

		down_write(&mm->mmap_sem);
		memory->waiting_for_main = NULL;
		mm->thread_op = prev;
		mm->distr_vma_op_counter--;

		mm->vma_operation_index++;
		if ( mm->vma_operation_index < 0 )
			printk("%s: WARN: vma_operation_index is underflow detected %d (cpu %d id %d)\n",
					__func__, mm->vma_operation_index, operation->origin_nid, operation->origin_pid);

		if (memory->my_lock != 1) {
			VSPRINTK("Released distributed lock\n");
			up_write(&mm->distribute_sem);
		}

		VSPRINTK("%s: INFO: CLIENT vma_operation_index is %d ENDING OP\n",
				__func__, mm->vma_operation_index);
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

	memory_t* entry = find_memory_entry(lock->origin_nid,
					    lock->origin_pid);
	vma_ack_t *ack_to_server;
	if (entry != NULL) {
		down_write(&entry->mm->distribute_sem);
		VSPRINTK("Acquired distributed lock\n");
		if (lock->from_cpu == my_nid)
			entry->my_lock = 1;
	}

	ack_to_server = (vma_ack_t*) kmalloc(sizeof(vma_ack_t), GFP_ATOMIC);
	BUG_ON(!ack_to_server);

	ack_to_server->origin_nid = lock->origin_nid;
	ack_to_server->origin_pid = lock->origin_pid;
	ack_to_server->vma_operation_index = lock->vma_operation_index;
	ack_to_server->header.type = PCN_KMSG_TYPE_PROC_SRV_VMA_ACK;
	ack_to_server->header.prio = PCN_KMSG_PRIO_NORMAL;

	pcn_kmsg_send_long(lock->origin_nid, ack_to_server, sizeof(*ack_to_server));

	kfree(ack_to_server);
	pcn_kmsg_free_msg(lock);
	kfree(work);
	return ;
}


static int handle_vma_ack(struct pcn_kmsg_message* inc_msg)
{
	vma_ack_t* ack = (vma_ack_t*) inc_msg;
	vma_op_answers_t* ack_holder;
	unsigned long flags;
	struct task_struct* task_to_wake_up = NULL;

	VSPRINTK("Vma ack received from cpu %d\n", ack->header.from_cpu);
	ack_holder = find_vma_ack_entry(ack->origin_nid, ack->origin_pid);
	if (ack_holder) {
		spin_lock_irqsave(&(ack_holder->lock), flags);

		ack_holder->responses++;
		ack_holder->address = ack->addr;

		if (ack_holder->vma_operation_index == -1)
			ack_holder->vma_operation_index = ack->vma_operation_index;
		else if (ack_holder->vma_operation_index != ack->vma_operation_index)
			printk("%s: ERROR: receiving an ack vma for a different operation index\n", __func__);

		if (ack_holder->responses >= ack_holder->expected_responses)
			task_to_wake_up = ack_holder->waiting;

		spin_unlock_irqrestore(&(ack_holder->lock), flags);

		if (task_to_wake_up)
			wake_up_process(task_to_wake_up);
	}

	pcn_kmsg_free_msg(inc_msg);
	return 1;
}

static int handle_vma_op(struct pcn_kmsg_message* inc_msg)
{
	vma_operation_t* operation = (vma_operation_t*) inc_msg;
	struct vma_op_work* work;

	//printk("Received an operation\n");

	memory_t* memory = find_memory_entry(operation->origin_nid,
					     operation->origin_pid);
	if (memory != NULL) {
		work = kmalloc(sizeof(struct vma_op_work), GFP_ATOMIC);
		BUG_ON(!work);
		work->fake = 0;
		work->memory = memory;
		work->operation = operation;
		INIT_WORK( (struct work_struct*)work, vma_server_process_vma_op);
		queue_work(vma_op_wq, (struct work_struct*) work);
	} else {
		if (operation->origin_nid == my_nid)
			printk("%s: ERROR: received an operation that said that I am the server but no memory_t found\n", __func__);
		else {
			printk("%s: WARN: Received an operation for a distributed process not present here\n", __func__);
		}
		pcn_kmsg_free_msg(inc_msg);
	}

	return 1;
}

/* which are the locks help on entering this function?
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

	//printk("%s: INFO: Ending distributed vma operation %i pid %d counter %d\n", __func__, operation,current->pid, current->mm->distr_vma_op_counter);
	if (current->mm->distr_vma_op_counter <= 0
	    || (current->is_vma_worker == 0 && current->mm->distr_vma_op_counter > 2)
	    || (current->is_vma_worker == 1 && current->mm->distr_vma_op_counter > 3))
		printk("%s:ERROR: exiting from a distributed vma operation with distr_vma_op_counter = %i\n",
				__func__, current->mm->distr_vma_op_counter);

	(current->mm->distr_vma_op_counter)--; // decrement the counter for nested operations

	if (operation == VMA_OP_MAP || operation == VMA_OP_BRK) {
		if (current->mm->was_not_pushed <= 0)
			printk("%s: ERROR: exiting from a mapping operation with was_not_pushed = %i\n",
					__func__, current->mm->was_not_pushed);
		current->mm->was_not_pushed--;
	}

	entry = find_memory_entry(current->origin_nid, current->origin_pid);
	if (entry == NULL) {
		printk("%s: ERROR: Cannot find message to send in exit operation\n", __func__);
		return;
	}
	if (start_ret == VMA_OP_SAVE) {
		int err;
		/*if (operation!=VMA_OP_MAP ||operation!=VMA_OP_REMAP ||operation!=VMA_OP_BRK )
		  printk("ERROR: asking for saving address from operation %i",operation);
		*/
		if (my_nid != current->origin_nid)
			printk("%s: ERROR: asking for saving address from a client", __func__);

		//now I have the new address I can send the message
		if (entry->message_push_operation != NULL) {
			switch (operation) {
			case VMA_OP_MAP:
			case VMA_OP_BRK:
				if (current->is_vma_worker == 0)
					printk("%s: ERROR: server not main asked to save operation\n", __func__);
				entry->message_push_operation->addr = addr;
				break;
			case VMA_OP_REMAP:
				entry->message_push_operation->new_addr = addr;
				break;
			default:
				printk("%s: ERROR: asking for saving address from a wrong operation\n", __func__);
				break;
			}
			up_write(&current->mm->mmap_sem);

			switch (operation) {
			case VMA_OP_MAP:
			case VMA_OP_BRK:
				err = pcn_kmsg_send_long(entry->message_push_operation->from_cpu,
										       (struct pcn_kmsg_long_message*) (entry->message_push_operation),
										       sizeof(vma_operation_t));
				if (err == -1)
					printk("%s: ERROR: impossible to send operation %d to client in cpu %d\n",
							__func__, operation, entry->message_push_operation->from_cpu);
				else {
					PSPRINTK("%s: INFO: operation %d sent to cpu %d\n",
							__func__, operation, entry->message_push_operation->from_cpu); }
				break;
			case VMA_OP_REMAP:
				PSPRINTK("%s: INFO: sending operation %d to all\n", __func__,operation);
				vma_send_long_all(entry, (entry->message_push_operation), sizeof(vma_operation_t), 0, 0);
				break;
			default:
				{} //no action taken here (before refactoring wasn't like this
			}

			down_write(&current->mm->mmap_sem);
			if (current->is_vma_worker == 0) {
				kfree(entry->message_push_operation);
				entry->message_push_operation = NULL;
			}
		}
		else { //here entry->message_push_operation == NULL
			printk("%s: ERROR: Cannot find message to send in exit operation (cpu %d id %d)\n",
					__func__, current->origin_nid, current->origin_pid);
		}
	}

/*****************************************************************************/
/* Here the message has been sent already in case of VMA_OP_SAVE             */
	if (current->mm->distr_vma_op_counter == 0) { // there are no nested operations (how I can be sure no one else it changing this here?
		current->mm->thread_op = NULL;
		entry->my_lock = 0;

		if (!(operation == VMA_OP_MAP || operation == VMA_OP_BRK)) { // operation is neither MAP nor BRK
			VSPRINTK("%s incrementing vma_operation_index\n", __func__);
			current->mm->vma_operation_index++;
			if (current->mm->vma_operation_index < 0)
				printk("%s: WARN: vma_operation_index underflow detected %d [if:if].(cpu %d id %d)\n",
						__func__, current->mm->vma_operation_index, current->origin_nid, current->origin_pid);
		}

		PSPRINTK("Releasing distributed lock\n");
		up_write(&current->mm->distribute_sem);

		if ( my_nid == current->origin_nid && !(operation == VMA_OP_MAP || operation == VMA_OP_BRK) ) {
			up_read(&entry->kernel_set_sem);
		}
		wake_up(&request_distributed_vma_op);
	}
	else { // there are nested operations --- not all situations are handled
		if (current->mm->distr_vma_op_counter == 1
		    && my_nid == current->origin_nid && current->is_vma_worker == 1) {

			if (!(operation == VMA_OP_MAP || operation == VMA_OP_BRK)){
				VSPRINTK("%s incrementing vma_operation_index\n", __func__);
				current->mm->vma_operation_index++;
				if (current->mm->vma_operation_index < 0)
					printk("%s: WARN: vma_operation_index underflow detected %d [else:if].(cpu %d id %d)\n",
							__func__, current->mm->vma_operation_index, current->origin_nid, current->origin_pid);

				up_read(&entry->kernel_set_sem);
			}

			PSPRINTK("Releasing distributed lock\n");
			up_write(&current->mm->distribute_sem);
		}
		else {
			if (!(current->mm->distr_vma_op_counter == 1
			      && my_nid != current->origin_nid && current->is_vma_worker == 1)) {

				//nested operation
				if (operation != VMA_OP_UNMAP)
					printk("%s: WARN: exiting from a nest operation that is %d (cpu %d id %d)\n",
							__func__, operation, current->origin_nid, current->origin_pid);

				//nested operation do not release the lock
				PSPRINTK("%s incrementing vma_operation_index\n", __func__);
				current->mm->vma_operation_index++;
				if (current->mm->vma_operation_index < 0)
					printk("%s: WARN: vma_operation_index underflow detected %d [else:else].(cpu %d id %d)\n",
							__func__, current->mm->vma_operation_index, current->origin_nid, current->origin_pid);
			}
		}
	}
	PSPRINTK("%s: operation index is %d\n", __func__, current->mm->vma_operation_index);
}

/* Marina: nesting is handled manually (?)
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
	int server = 1;
	if (current->origin_nid != my_nid)
		server = 0;

	//set default return value
	if (server)
		ret = VMA_OP_NOT_SAVE;
	else if (operation == VMA_OP_REMAP)
		ret = new_addr;

// TODO review nesting
	/*All the operation pushed by the server are executed as not distributed in clients*/
	if (current->mm->distribute_unmap == 0) {
		PSPRINTK(KERN_ALERT"%s: INFO: vma operation for pid %i "
				"origin_nid %i origin_pid %i main %d operation %i "
				"addr %lx len %lx end %lx RETURNING\n", __func__,
				current->pid, current->origin_nid,
				current->origin_pid, current->is_vma_worker ? 1 : 0,
				operation, addr, len, addr+len);
		return ret;
	}
	/*printk("%s: INFO: pid %d origin_nid %d origin_pid %d main %d operation %s addr %lx len %lx end %lx index %d\n",
		    __func__, current->pid, current->origin_nid, current->origin_pid, current->is_vma_worker?1:0,
		    (operation == VMA_OP_NOP) ? "NOP" : ((operation == VMA_OP_UNMAP) ? "UNMAP" : ((operation == VMA_OP_PROTECT) ? "PROTECT" : (
		    						(operation == VMA_OP_REMAP) ? "REMAP" : ((operation == VMA_OP_MAP) ? "MAP" : ((operation == VMA_OP_BRK) ? "BRK" : "?"))))),
		    addr, len, addr+len, current->mm->vma_operation_index);*/

	/*only server can have legal distributed nested operations*/
	if ((current->mm->distr_vma_op_counter > 0) && (current->mm->thread_op == current)) {
		PSPRINTK("%s, Recursive operation\n", __func__);

		if (server == 0
				|| (current->is_vma_worker == 0 && current->mm->distr_vma_op_counter > 1)
				|| (current->is_vma_worker == 0 && operation != VMA_OP_UNMAP)) {
			printk("%s: ERROR: invalid nested vma operation %d (counter %d)\n",
					__func__, operation, current->mm->distr_vma_op_counter);
			return -EPERM;
		} else {
			/*the main executes the operations for the clients
			 *distr_vma_op_counter is already increased when it start the operation*/
			if (current->is_vma_worker == 1) {
				VSPRINTK("%s, I am the main, so it maybe not a real recursive operation...\n", __func__); // what does this mean?

				if (current->mm->distr_vma_op_counter < 1 // who change the value in the meantime?
						|| current->mm->distr_vma_op_counter > 2
						|| (current->mm->distr_vma_op_counter == 2 && operation != VMA_OP_UNMAP)) {
					printk("%s: ERROR: invalid nested vma operation in main server, operation %d (counter %d)\n",
							__func__, operation, current->mm->distr_vma_op_counter);
					return -EPERM;
				} else {
					//current->mm->distr_vma_op_counter++;

					if (current->mm->distr_vma_op_counter == 2) {
						current->mm->distr_vma_op_counter++;
						VSPRINTK("%s, Recursive operation for the main\n", __func__);
						/*in this case is a nested operation on main
						 * if the previous operation was a pushed operation
						 * do not distribute it again*/
						if (current->mm->was_not_pushed == 0) {
							VSPRINTK("%s, don't distribute again, return!\n", __func__);
							return ret;
						} else {
							current->mm->distr_vma_op_counter++;
							goto start;
						}
					} else {
						current->mm->distr_vma_op_counter++;
						goto start;
					}
				}
			} else { // not the main thread
				if (current->mm->was_not_pushed == 0) {
					current->mm->distr_vma_op_counter++; // this is decremented in end_distribute_operation
					VSPRINTK("%s, don't distribute again, return!\n",__func__);
					return ret;
				} else {
					current->mm->distr_vma_op_counter++;
					goto start;
				}
			}
		}
	} //Antonio: all the possibility are handled but I am not sure their handled correctly

	/* I did not start an operation, but another thread maybe did...
	 * => no concurrent operations of the same process on the same kernel*/
	if (current->mm->distr_vma_op_counter < 0)
		printk(KERN_ALERT"%s: ERROR: distr_vma_op_counter is %d", __func__,
				current->mm->distr_vma_op_counter);
	// ONLY ONE CAN BE IN DISTRIBUTED OPERATION AT THE TIME ... WHAT ABOUT NESTED OPERATIONS?
	while (atomic_cmpxchg((atomic_t*)&(current->mm->distr_vma_op_counter), 0, 1) != 0) { //success is indicated by comparing RETURN with OLD (arch/x86/include/asm/cmpxchg.h)
		DEFINE_WAIT(wait);
		up_write(&current->mm->mmap_sem);

		prepare_to_wait(&request_distributed_vma_op, &wait, TASK_UNINTERRUPTIBLE);
		if (current->mm->distr_vma_op_counter > 0) {
			printk("%s: LOCK: Somebody already started a distributed operation "
					"(current->mm->thread_op->pid is %d). "
					"I am pid %d and I am going to sleep (cpu %d id %d)\n",
					__func__, current->mm->thread_op->pid, current->pid,
					current->origin_nid, current->origin_pid);
			schedule();
		}
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
		if (current->is_vma_worker == 1 && !(current->mm->distr_vma_op_counter>2)) {
			/* I am the main thread=> a client asked me to do an operation. */
			int error;
			int index = current->mm->vma_operation_index; //(current->mm->vma_operation_index)++;
			memory_t* entry;
			PSPRINTK("SERVER MAIN: starting operation %d, current index is %d\n", operation, index);

			up_write(&current->mm->mmap_sem);
			entry = find_memory_entry(current->origin_nid, current->origin_pid);
			if (entry == NULL || entry->message_push_operation == NULL) {
				printk("%s: ERROR: Mapping disappeared or cannot find message to update(cpu %d id %d)\n",
						__func__, current->origin_nid, current->origin_pid);
				down_write(&current->mm->mmap_sem);
				ret = -ENOMEM;
				goto out;
			}

/*****************************************************************************/
/* Locking and Acking                                                        */
/*****************************************************************************/
			{
			vma_op_answers_t* acks;
			unsigned long flags;
			//First: send a message to everybody to acquire the lock to block page faults
			vma_lock_t* lock_message = vma_lock_alloc(current, entry->message_push_operation->from_cpu, index);
			if (lock_message == NULL) {
				down_write(&current->mm->mmap_sem);
				ret = -ENOMEM;
				goto out;
			}

			acks = vma_op_answer_alloc(current, index);
			if (acks == NULL) {
				kfree(lock_message);
				down_write(&current->mm->mmap_sem);
				ret = -ENOMEM;
				goto out;
			}

			/*Partial replication: mmap and brk need to communicate only between server and one client
			 * */
			if (operation == VMA_OP_MAP || operation == VMA_OP_BRK) {
				error = pcn_kmsg_send_long(
						entry->message_push_operation->from_cpu,
						lock_message, sizeof(vma_lock_t));
				if (error != -1)
					acks->expected_responses++;
			} else {
				down_read(&entry->kernel_set_sem);
				acks->expected_responses = vma_send_long_all(
						entry, lock_message, sizeof(vma_lock_t), current, 2);
			}

			while (acks->expected_responses != acks->responses) {
				set_task_state(current, TASK_UNINTERRUPTIBLE);
				if (acks->expected_responses != acks->responses) {
					schedule();
				}
				set_task_state(current, TASK_RUNNING);
			}
			PSPRINTK("SERVER MAIN: Received all ack to lock\n");

			spin_lock_irqsave(&(acks->lock), flags);
			spin_unlock_irqrestore(&(acks->lock), flags);
			remove_vma_ack_entry(acks);
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
			if (operation == VMA_OP_UNMAP || operation == VMA_OP_PROTECT
			    || ((operation == VMA_OP_REMAP) && (flags & MREMAP_FIXED))) {
				PSPRINTK("%s: INFO: SERVER MAIN we can execute the operation in parallel! %d %lx %lx\n",
						__func__, operation, flags, flags & MREMAP_FIXED);

				vma_send_long_all(entry, (entry->message_push_operation), sizeof(vma_operation_t), 0, 0);
				down_write(&current->mm->mmap_sem);
				return ret;
			}
			else {
				PSPRINTK("%s: INFO: SERVER MAIN going to execute the operation locally %d\n",
						__func__, operation);

				down_write(&current->mm->mmap_sem);
				return VMA_OP_SAVE;
			}
		}
//SERVER not main//////////////////////////////////////////////////////////////
		else {
			int index;
			memory_t *entry;
			vma_operation_t* operation_to_send;

			if (current->is_vma_worker != 0) ///ERROR IF I AM NOT MAIN - do this check because there can be a possibility of >2 counter
				printk(KERN_ALERT"%s: WARN?ERROR: Server not main operation but curr->is_vma_worker is %d\n",
					__func__, current->is_vma_worker);

			PSPRINTK("%s: SERVER NOT MAIN starting operation %d for pid %d current index is %d\n",
					__func__, operation, current->pid, current->mm->vma_operation_index);

			switch (operation) {
			case VMA_OP_MAP:
			case VMA_OP_BRK:
				//if I am the server, mmap and brk can be executed locally
				VSPRINTK("%s pure local operation!\n", __func__);
				//Note: the order in which locks are taken is important
				up_write(&current->mm->mmap_sem);

				down_write(&current->mm->distribute_sem);
				VSPRINTK("Distributed lock acquired\n");
				down_write(&current->mm->mmap_sem);

				//(current->mm->vma_operation_index)++;
				return ret;
			default:
				break;
			}

			//new push-operation
			PSPRINTK("%s push operation!\n", __func__);
			//(current->mm->vma_operation_index)++;
			index = current->mm->vma_operation_index;
			PSPRINTK("current index is %d\n", index);

			/*Important: while I am waiting for the acks to the LOCK message
			 * mmap_sem have to be unlocked*/
			up_write(&current->mm->mmap_sem);
			entry = find_memory_entry(current->origin_nid, current->origin_pid);
			if (entry==NULL) {
				printk("%s: ERROR: Mapping disappeared, cannot save message to update by exit_distribute_operation (cpu %d id %d)\n",
						__func__, current->origin_nid, current->origin_pid);
				down_write(&current->mm->mmap_sem);
				ret = -EPERM;
				goto out;
			}

/*****************************************************************************/
/* Locking and Acking                                                        */
/*****************************************************************************/
			{
			vma_op_answers_t *acks;
			unsigned long flags;

			/*First: send a message to everybody to acquire the lock to block page faults*/
			vma_lock_t* lock_message = vma_lock_alloc(current, my_nid, index);
			if (lock_message == NULL) {
				down_write(&current->mm->mmap_sem);
				ret = -ENOMEM;
				goto out;
			}

			acks = vma_op_answer_alloc(current, index);
			if (acks == NULL) {
				kfree(lock_message);
				down_write(&current->mm->mmap_sem);
				ret = -ENOMEM;
				goto out;
			}

//ANTONIOB: why here we are not distinguish between different type of operations?
			down_read(&entry->kernel_set_sem);
			acks->expected_responses = vma_send_long_all(
					entry, lock_message, sizeof(vma_lock_t), 0, 0);

			/*Second: wait that everybody acquire the lock, and acquire it locally too*/
			while (acks->expected_responses != acks->responses) {
				set_task_state(current, TASK_UNINTERRUPTIBLE);
				if (acks->expected_responses != acks->responses) {
					schedule();
				}
				set_task_state(current, TASK_RUNNING);
			}
			PSPRINTK("SERVER NOT MAIN: Received all ack to lock\n");

			spin_lock_irqsave(&(acks->lock), flags);
			spin_unlock_irqrestore(&(acks->lock), flags);
			remove_vma_ack_entry(acks);
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
			if (operation_to_send == NULL) {
				if (current->mm->distr_vma_op_counter == 1)
					up_write(&current->mm->distribute_sem);
				down_write(&current->mm->mmap_sem);
				up_read(&entry->kernel_set_sem);
				ret = -ENOMEM;
				goto out;
			}

			/* Third: push the operation to everybody
			 * If the operation was a remap without fixed parameters, I cannot let other kernels
			 * locally choose where to remap it =>
			 * I need to push what the server choose as parameter to the other an push the operation with
			 * a fixed flag.
			 * */
			if (!(operation == VMA_OP_REMAP) || (flags & MREMAP_FIXED)) {
				PSPRINTK("%s: INFO: SERVER NOT MAIN "
						"sending done for operation, "
						"we can execute the operation in parallel! %d\n",
						__func__, operation);

				vma_send_long_all(
						entry, operation_to_send,sizeof(vma_operation_t), 0, 0);
				kfree(operation_to_send);
				down_write(&current->mm->mmap_sem);
				return ret;
			} else {
				PSPRINTK("%s: INFO: SERVER NOT MAIN "
						"going to execute the operation locally %d\n",
						__func__, operation);
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

		PSPRINTK("%s: INFO: CLIENT starting operation %i for pid %d "
				"current index is%d\n", __func__,
				operation, current->pid, current->mm->vma_operation_index);

		/*First: send the operation to the server*/
		operation_to_send = vma_operation_alloc(current, operation,
				addr, new_addr, len, new_len, prot, flags, -1);
		if (operation_to_send == NULL) {
			ret = -ENOMEM;
			goto out;
		}
		operation_to_send->pgoff = pgoff;
		if (file != NULL) {
			char path[256] = { 0 };
			char* rpath;
			rpath = d_path(&file->f_path, path, 256);
			strcpy(operation_to_send->path, rpath);
		} else
			operation_to_send->path[0] = '\0';

		/*In this case the server will eventually send me the push operation.
		 *Differently from a not-started-by-me push operation, it is not the main thread that has to execute it,
		 *but this thread has.
		 */
		entry = find_memory_entry(current->origin_nid, current->origin_pid);
		if (entry) {
			if (entry->waiting_for_op != NULL) {
				printk("%s: ERROR: Somebody is already waiting for an op "
						"(cpu %d id %d)\n", __func__,
						current->origin_nid, current->origin_pid);
				kfree(operation_to_send);
				ret = -EPERM;
				goto out;
			}
			entry->waiting_for_op = current;
			entry->arrived_op = 0;
		} else {
			printk("%s: ERROR: Mapping disappeared, "
					"cannot wait for push op (cpu %d id %d)\n", __func__,
					current->origin_nid, current->origin_pid);
			kfree(operation_to_send);
			ret = -EPERM;
			goto out;
		}

		up_write(&current->mm->mmap_sem);
		//send the operation to the server
		error = pcn_kmsg_send_long(current->origin_nid,
					   (struct pcn_kmsg_long_message*) (operation_to_send),
					   sizeof(vma_operation_t));
		if (error == -1) {
			printk("%s: ERROR: Impossible to contact the server", __func__);
			kfree(operation_to_send);
			down_write(&current->mm->mmap_sem);
			ret = -EPERM;
			goto out;
		}

		/*Second: the server will send me a LOCK message... another thread will handle it...*/
		/*Third: wait that the server push me the operation*/
		while (entry->arrived_op == 0) {
			set_task_state(current, TASK_UNINTERRUPTIBLE);
			if (entry->arrived_op == 0) {
				schedule();
			}
			set_task_state(current, TASK_RUNNING);
		}
		PSPRINTK("My operation finally arrived pid %d vma operation %d\n",
				current->pid,current->mm->vma_operation_index);

		/*Note, the distributed lock already has been acquired*/
		down_write(&current->mm->mmap_sem);

		if (current->mm->thread_op != current) {
			printk(	"%s: ERROR: waking up to locally execute a vma operation "
					"started by me, but thread_op s not me\n", __func__);
			kfree(operation_to_send);
			ret = -EPERM;
			goto out_dist_lock;
		}

		if (operation == VMA_OP_REMAP || operation == VMA_OP_MAP
		    || operation == VMA_OP_BRK) {
			ret = entry->addr;
			if (entry->addr < 0) {
				printk("%s: WARN: Received error %lx from the server for "
						"operation %d\n", __func__, ret,operation);
				goto out_dist_lock;
			}
		}

		entry->waiting_for_op = NULL;
		kfree(operation_to_send);
		return ret;
	}

out_dist_lock:
	up_write(&current->mm->distribute_sem);
	PSPRINTK("%s: Released distributed lock from out_dist_lock, "
			"current index is %d in out_dist_lock\n",
			__func__, current->mm->vma_operation_index);

out:
	current->mm->distr_vma_op_counter--;
	current->mm->thread_op = NULL;
	if (operation == VMA_OP_MAP || operation == VMA_OP_BRK) {
		current->mm->was_not_pushed--;
	}

	wake_up(&request_distributed_vma_op);
	return ret;
}


int vma_server_enqueue_vma_op(
		memory_t * memory, vma_operation_t * operation, int fake)
{
	struct vma_op_work *work = kmalloc(sizeof(struct vma_op_work), GFP_ATOMIC);
	if (!work) return -ENOMEM;

	work->fake = fake;
	work->memory = memory;
	work->operation = operation;

	INIT_WORK((struct work_struct *)work, vma_server_process_vma_op);
	queue_work(vma_op_wq, (struct work_struct *)work);

	return 0;
}


#define GET_UNMAP_IF_HOME(task, memory) { \
if (task->tgroup_home_cpu != my_nid) { \
	WARN(memory->mm->distribute_unmap == 0, \
		"GET_UNMAP_IF_HOME: value was already 0, check who is the older.\n"); \
	memory->mm->distribute_unmap = 0; \
	} \
}
#define PUT_UNMAP_IF_HOME(task, memory) { \
if (task->tgroup_home_cpu != my_nid) {\
	WARN(memory->mm->distribute_unmap == 1, \
		"PUT_UNMAP_IF_HOME: value was already 1, check who is the older.\n"); \
	memory->mm->distribute_unmap = 1; \
	} \
}

extern long madvise_remove(struct vm_area_struct *vma,
		struct vm_area_struct **prev, unsigned long start, unsigned long end);
extern int kernel_mprotect(unsigned long start, size_t len, unsigned long prot);
extern long kernel_mremap(unsigned long addr, unsigned long old_len,
		unsigned long new_len, unsigned long flags, unsigned long new_addr);


/**
 * We do this stupid thing because functions related to meomry mapping operate
 * on "current". Thus, we need mmap/munmap/madvise in our process
 */
void vma_worker_main(struct remote_context *rc, const char *at)
{
	might_sleep();

	PSPRINTK("%s [%d]: at %s\n", __func__, current->pid, at);

	while (!kthread_should_stop()) {
		/*
		while (memory->operation != VMA_OP_NOP &&
		       memory->mm->thread_op == current) {
			unsigned long ret = 0;

			switch (memory->operation) {
			case VMA_OP_UNMAP:
				down_write(&memory->mm->mmap_sem);
				GET_UNMAP_IF_HOME(current, memory);
				ret = do_munmap(memory->mm, memory->addr, memory->len);
				PUT_UNMAP_IF_HOME(current, memory);
				up_write(&memory->mm->mmap_sem);
				break;

			case VMA_OP_MADVISE: { //this is only for MADV_REMOVE (thus write is 0)
				struct vm_area_struct *pvma;
				struct vm_area_struct *vma = find_vma(memory->mm, memory->addr);
				if (!vma ||(vma->vm_start > memory->addr || vma->vm_end < memory->addr))
					printk("%s: ERROR VMA_OP_MADVISE cannot find VMA "
							"addr %lx start %lx end %lx\n", __func__,
							memory->addr, (vma ? vma->vm_start : 0),
							(vma ? vma->vm_end : 0));
				//write = madvise_need_mmap_write(behavior);
				//if (write)
				//	down_write(&(memory->mm->mmap_sem));
				//else
				down_read(&(memory->mm->mmap_sem));
				GET_UNMAP_IF_HOME(current, memory);
				ret = madvise_remove(vma, &pvma,
						memory->addr, (memory->addr + memory->len));
				PUT_UNMAP_IF_HOME(current, memory);
				//if (write)
				//	up_write(&(memory->mm->mmap_sem));
				//else
				up_read(&(memory->mm->mmap_sem));
				break;
			}

			case VMA_OP_PROTECT:
				GET_UNMAP_IF_HOME(current, memory);
				ret = kernel_mprotect(memory->addr, memory->len, memory->prot);
				PUT_UNMAP_IF_HOME(current, memory);
				break;

			case VMA_OP_REMAP:
				// note that remap calls unmap --- thus is a nested operation ...
				down_write(&memory->mm->mmap_sem);
				GET_UNMAP_IF_HOME(current, memory);
				ret = kernel_mremap(memory->addr, memory->len, memory->new_len, 0, memory->new_addr);
				PUT_UNMAP_IF_HOME(current, memory);
				up_write(&memory->mm->mmap_sem);
				break;

			case VMA_OP_BRK:
				ret = -1;
				down_write(&memory->mm->mmap_sem);
				GET_UNMAP_IF_HOME(current, memory);
				ret = do_brk(memory->addr, memory->len);
				PUT_UNMAP_IF_HOME(current, memory);
				up_write(&memory->mm->mmap_sem);
				break;

			case VMA_OP_MAP: {
				struct file *f = NULL;
				unsigned long populate = 0;
				ret = -1;
				if (memory->path[0] != '\0') {
					f = filp_open(memory->path, O_RDONLY | O_LARGEFILE, 0);
					if (IS_ERR(f)) {
						printk("ERROR: cannot open file to map\n");
						break;
					}
				}
				down_write(&memory->mm->mmap_sem);
				GET_UNMAP_IF_HOME(current, memory);
				ret = do_mmap_pgoff(f, memory->addr, memory->len, memory->prot,
						    memory->flags, memory->pgoff, &populate);
				PUT_UNMAP_IF_HOME(current, memory);
				up_write(&memory->mm->mmap_sem);

				if (memory->path[0] != '\0') {
					filp_close(f, NULL);
				}
				break;
			}

			default:
				break;
			}
			memory->addr = ret;
			memory->operation = VMA_OP_NOP;

            PSPRINTK("%s: wake up %d (cpu %d id %d)\n", __func__,
					memory->waiting_for_main->pid,
					current->origin_nid, current->origin_pid);
			wake_up_process(memory->waiting_for_main);
		}

		if ( (memory->operation != VMA_OP_NOP &&
			 memory->mm->thread_op == current)) {
			__set_task_state(current, TASK_RUNNING);
			continue;
		}
		*/
		__set_task_state(current, TASK_INTERRUPTIBLE);
		schedule_timeout(HZ * 30);
		__set_task_state(current, TASK_RUNNING);
	}

	printk("%s [%d]: %s exited\n", __func__, current->pid, at);

	return;
}



/**
 * Response for remote page request and handling the response
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

	rcu_read_lock();
	tsk = find_task_by_vpid(res->remote_pid);
	if (!tsk) {
		__WARN();
		rcu_read_unlock();
		goto out_free;
	}
	get_task_struct(tsk);
	rcu_read_unlock();
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

	VSPRINTK("%s: %d, %d pended\n", __func__,
			res->result, atomic_read(&vi->pendings));
	vi->response = res;
	wake_up(&vi->pendings_wait);

out_free:
	kfree(w);
}


static void response_remote_vma(int remote_nid, int remote_pid, remote_vma_response_t *res)
{
	res->header.type = PCN_KMSG_TYPE_REMOTE_VMA_RESPONSE;
	res->header.prio = PCN_KMSG_PRIO_NORMAL;

	res->remote_pid = remote_pid;

	pcn_kmsg_send_long(remote_nid, res, sizeof(*res));
}


/**
 * Request for remote pages and handling the request
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
	char path[512];
	char *ppath;

	might_sleep();

	while (!res) {
		res = kmalloc(sizeof(*res), GFP_KERNEL);
	}
	res->addr = addr;

	rcu_read_lock();
	tsk = find_task_by_vpid(req->origin_pid);
	if (!tsk) {
		printk("remote_vma:: process does not exist %d\n", req->origin_pid);
		res->result = -ESRCH;
		rcu_read_unlock();
		goto out_free;
	}
	get_task_struct(tsk);
	rcu_read_unlock();

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

	if (vma->vm_file) {
		/* File-mapped page */
		ppath = d_path(&vma->vm_file->f_path, path, sizeof(path));
		strncpy(res->vm_file_path, ppath, sizeof(res->vm_file_path));
	} else {
		res->vm_file_path[0] = '\0';
	}
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
	VSPRINTK("## VMAFAULT [%d]: %lx %lx\n",
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
			VSPRINTK("%s [%d]: %lx from %d at %d\n", __func__,
					current->pid, addr, tsk->origin_pid, tsk->origin_nid);
			smp_wmb();
			list_add(&vi->list, &rc->vmas);
		} else {
			printk("%s [%d]: %lx already pended\n", __func__,
					current->pid, addr);
			kfree(vi);
			vi = v;
			kfree(req);
			req = NULL;
		}
	} else {
		VSPRINTK("%s [%d]: %lx already pended\n", __func__, current->pid, addr);
	}
	atomic_inc(&vi->pendings);
	prepare_to_wait(&vi->pendings_wait, &wait, TASK_UNINTERRUPTIBLE);
	spin_unlock_irqrestore(&rc->vmas_lock, flags);

	if (req) {
		pcn_kmsg_send_long(tsk->origin_nid, req, sizeof(*req));
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

	VSPRINTK("%s [%d]: %lx resume %d\n", __func__, current->pid, addr, ret);

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
	smp_wmb();

	if (wakeup) wake_up(&vi->pendings_wait);

	return ret;
}


DEFINE_KMSG_WQ_HANDLER(remote_vma_request, vma_op_wq);
DEFINE_KMSG_WQ_HANDLER(remote_vma_response, vma_op_wq);
DEFINE_KMSG_WQ_HANDLER(vma_lock, vma_op_wq);

int vma_server_init(void)
{
	/*
	 * These two workqueues are singlethread because we ran into
	 * synchronization issues using multithread workqueues.
	 */
	vma_op_wq = create_singlethread_workqueue("vma_op_wq");
	if (!vma_op_wq) return -ENOMEM;

	pcn_kmsg_register_callback(
			PCN_KMSG_TYPE_PROC_SRV_VMA_OP, handle_vma_op);
	pcn_kmsg_register_callback(
			PCN_KMSG_TYPE_PROC_SRV_VMA_ACK, handle_vma_ack);
	REGISTER_KMSG_WQ_HANDLER(
			PCN_KMSG_TYPE_PROC_SRV_VMA_LOCK, vma_lock);

	/* sanghoon: sanitized */
	REGISTER_KMSG_WQ_HANDLER(
			PCN_KMSG_TYPE_REMOTE_VMA_REQUEST, remote_vma_request);
	REGISTER_KMSG_WQ_HANDLER(
			PCN_KMSG_TYPE_REMOTE_VMA_RESPONSE, remote_vma_response);

	return 0;
}
