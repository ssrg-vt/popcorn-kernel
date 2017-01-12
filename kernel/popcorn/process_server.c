/**
 * @file process_server.c
 *
 * Popcorn Linux Migration server implementation
 * This work is an extension of David Katz MS Thesis, please refer to the
 * Thesis for further information about the algorithm.
 *
 * @author Antonio Barbalace, SSRG Virginia Tech 2016
 * @author Vincent Legout, Antonio Barbalace, Sharat Kumar Bath, Ajithchandra Saya, SSRG Virginia Tech 2014-2015
 * @author David Katz, Marina Sadini, SSRG Virginia 2013
 */

#include <linux/delay.h>
#include <linux/sched.h>
#include <linux/threads.h>
#include <linux/slab.h>
#include <linux/kthread.h>
#include <linux/sched.h>

#include <asm/mmu_context.h>

#define  NSIG 32

#include <linux/cpu_namespace.h>
#include <linux/popcorn_cpuinfo.h>
#include <linux/process_server.h>

#include <process_server_arch.h>
#include "stat.h"
#include "internal.h"
#include "vma_server.h"

/*
#include <popcorn/process_server.h>
#include "vma_server.h"
#include "page_server.h"
#include <popcorn/page_server.h>
#include "sched_server.h"
#include <popcorn/sched_server.h>
#include <popcorn/remote_file.h>

#include <linux/elf.h>
#include <linux/binfmts.h>
#include <asm/elf.h>

#include "../futex_remote.h"
*/

/* all the above are implemented in internal.h */

/*akshay*/
//#pragma GCC diagnostic ignored "-Wdeclaration-after-statement"

int _cpu = 0;		// This is for any Popcorn Linux server

#if 0 // beowulf
int _file_cpu = 1;	// This is for the Popcorn Linux file server
#endif

///////////////////////////////////////////////////////////////////////////////
// Marina's data stores (linked lists)
///////////////////////////////////////////////////////////////////////////////

mapping_answers_for_2_kernels_t* _mapping_head = NULL;
DEFINE_SPINLOCK(_mapping_head_lock);

ack_answers_for_2_kernels_t* _ack_head = NULL;
DEFINE_SPINLOCK(_ack_head_lock);

LIST_HEAD(_memory_head);
DEFINE_SPINLOCK(_memory_head_lock);

LIST_HEAD(_count_head);
DEFINE_SPINLOCK(_count_head_lock);

LIST_HEAD(_vma_ack_head);
DEFINE_SPINLOCK(_vma_ack_head_lock);


static void __rename_task_comm(struct task_struct *task, char *name)
{
	int i, ch;
	char comm[TASK_COMM_LEN];

	for (i = 0; (ch = *(name++)) != '\0';) {
		if (ch == '/')
			i = 0;
		else if (i < (sizeof(comm) - 1))
			comm[i++] = ch;
	}
	comm[i] = '\0';
	set_task_comm(task, comm);
}

/*
 * Create a kernel thread -  required for process server to create a 
 * kernel thread from user context.
 */
pid_t kernel_thread_popcorn(int (*fn)(void *), void *arg)
{
	const unsigned long flags = CLONE_THREAD | CLONE_SIGHAND | 
						  CLONE_VM | CLONE_UNTRACED | SIGCHLD;
	const unsigned long flags_current = current->flags;
	pid_t pid;

	current->flags |= PF_KTHREAD;
	pid = kernel_thread(fn, arg, flags);
	current->flags = flags_current;

	return pid;
}


///////////////////////////////////////////////////////////////////////////////
// Working queues (servers)
///////////////////////////////////////////////////////////////////////////////
static struct workqueue_struct *clone_wq;
static struct workqueue_struct *exit_wq;
static struct workqueue_struct *exit_group_wq;
static struct workqueue_struct *new_kernel_wq;


static int count_remote_thread_members(int tgroup_home_cpu, int tgroup_home_id, memory_t* memory)
{
	count_answers_t* data;
	remote_thread_count_request_t* req;
	int ret = -1;
	_remote_cpu_info_list_t *r;

	data = kmalloc(sizeof(*data), GFP_ATOMIC);
	BUG_ON(!data);

	data->responses = 0;
	data->tgroup_home_cpu = tgroup_home_cpu;
	data->tgroup_home_id = tgroup_home_id;
	data->count = 0;
	data->waiting = current;
	spin_lock_init(&(data->lock));

	add_count_entry(data);

	req = kmalloc(sizeof(*req),GFP_ATOMIC);
	BUG_ON(!req);

	req->header.type = PCN_KMSG_TYPE_PROC_SRV_THREAD_COUNT_REQUEST;
	req->header.prio = PCN_KMSG_PRIO_NORMAL;
	req->tgroup_home_cpu = tgroup_home_cpu;
	req->tgroup_home_id = tgroup_home_id;

	data->expected_responses = 0;

	down_read(&memory->kernel_set_sem);
	list_for_each_entry(r, &rlist_head, cpu_list_member) {
		int i = r->_data._processor;
		if (memory->kernel_set[i] == 1) {
			// Send the request to this cpu.
			//s = pcn_kmsg_send(i, (struct pcn_kmsg_message*) (&request));
			int sent = pcn_kmsg_send_long(i, req, sizeof(*req));
			if (sent !=-1 ) {
				// A successful send operation, increase the number
				// of expected responses.
				data->expected_responses++;
			}
		}
	}

	up_read(&memory->kernel_set_sem);
	
	//printk("%s going to sleep\n");
	while (data->expected_responses != data->responses) {
		set_task_state(current, TASK_UNINTERRUPTIBLE);
		if (data->expected_responses != data->responses)
			schedule();

		set_task_state(current, TASK_RUNNING);
	}
	//printk("%s waked up\n");
	// OK, all responses are in, we can proceed.
	//printk("%s data->count is %d",__func__,data->count);
	
	// Does not include the current processor group's count (TODO)
	ret = data->count;

	remove_count_entry(data);
	kfree(data);
	kfree(req);
	return ret;
}

static void process_new_kernel_answer(struct work_struct *_work)
{
	int i;
	new_kernel_work_answer_t* work = (new_kernel_work_answer_t*)_work;
	new_kernel_answer_t* answer = work->answer;
	memory_t* memory = work->memory;

	if (answer->header.from_cpu == answer->tgroup_home_cpu) {
		down_write(&memory->mm->mmap_sem);
		//	printk("%s answer->vma_operation_index %d NR_CPU %d\n",__func__,answer->vma_operation_index,MAX_KERNEL_IDS);
		memory->mm->vma_operation_index = answer->vma_operation_index;
		up_write(&memory->mm->mmap_sem);
	}

	down_write(&memory->kernel_set_sem);
	for (i = 0; i < MAX_KERNEL_IDS ; i++) {
		memory->kernel_set[i] |= answer->my_set[i];
	}
	up_write(&memory->kernel_set_sem);

	if (atomic_dec_return(&memory->answers_remain) == 0) {
		PSPRINTK("%s: wake up %d (%s)\n", __func__,
				memory->helper->pid, memory->helper->comm);
		wake_up_process(memory->helper);
	}

	pcn_kmsg_free_msg(answer);
	kfree(work);
}

///////////////////////////////////////////////////////////////////////////////
// handling a new incoming migration request?
///////////////////////////////////////////////////////////////////////////////
static int handle_new_kernel_answer(struct pcn_kmsg_message* inc_msg)
{
	new_kernel_answer_t* answer = (new_kernel_answer_t*)inc_msg;
	memory_t* memory = find_memory_entry(answer->tgroup_home_cpu,
					    answer->tgroup_home_id);

	PSNEWTHREADPRINTK("received new kernel answer\n");
	//printk("%s: %d\n",__func__,answer->vma_operation_index);
	if (memory != NULL) {
		new_kernel_work_answer_t* work = kmalloc(sizeof(*work), GFP_ATOMIC);
		BUG_ON(!work);
		work->answer = answer;
		work->memory = memory;
		INIT_WORK((struct work_struct*)work, process_new_kernel_answer);
		queue_work(new_kernel_wq, (struct work_struct*)work);
	} else {
		printk("%s: ERROR: received an answer for new kernel but memory_t not present cpu %d id %d\n",
				__func__, answer->tgroup_home_cpu, answer->tgroup_home_id);
		pcn_kmsg_free_msg(inc_msg);
	}
	
	return 1;
}

static void process_new_kernel(struct work_struct *_work)
{
	new_kernel_work_t* work = (new_kernel_work_t*)_work;
	new_kernel_t *req = work->request;
	memory_t* memory;
	new_kernel_answer_t* answer = kmalloc(sizeof(*answer), GFP_ATOMIC);
	BUG_ON(!answer);

	printk("%s: request cpu %d id %d\n", __func__,
			req->tgroup_home_cpu, req->tgroup_home_id);

	memory = find_memory_entry(req->tgroup_home_cpu, req->tgroup_home_id);
	if (memory != NULL) {
		printk("memory present cpu %d id %d\n",
				req->tgroup_home_cpu, req->tgroup_home_id);	
		down_write(&memory->kernel_set_sem);
		memory->kernel_set[req->header.from_cpu] = 1;
		memcpy(answer->my_set, memory->kernel_set, sizeof(answer->my_set));
		up_write(&memory->kernel_set_sem);

		if (_cpu == req->tgroup_home_cpu) {
			down_read(&memory->mm->mmap_sem);
			answer->vma_operation_index = memory->mm->vma_operation_index;
			up_read(&memory->mm->mmap_sem);
		}
	} else {
		PSPRINTK("memory not present\n");
		memset(answer->my_set, 0, sizeof(answer->my_set));
	}

	answer->header.type = PCN_KMSG_TYPE_PROC_SRV_NEW_KERNEL_ANSWER;
	answer->header.prio = PCN_KMSG_PRIO_NORMAL;

	answer->tgroup_home_cpu = req->tgroup_home_cpu;
	answer->tgroup_home_id = req->tgroup_home_id;

	pcn_kmsg_send_long(req->header.from_cpu, answer, sizeof(*answer));

	kfree(answer);
	kfree(work);
	pcn_kmsg_free_msg(req);
}

static int handle_new_kernel(struct pcn_kmsg_message* inc_msg)
{
	new_kernel_t* req= (new_kernel_t*)inc_msg;
	new_kernel_work_t* work;

	work = kmalloc(sizeof(new_kernel_work_t), GFP_ATOMIC);

	if (work) {
		work->request = req;
		INIT_WORK( (struct work_struct*)work, process_new_kernel);
		queue_work(new_kernel_wq, (struct work_struct*)work);
	}

	return 1;
}

///////////////////////////////////////////////////////////////////////////////
// thread pool functionalities
///////////////////////////////////////////////////////////////////////////////

/* return type:
 * 0 normal;
 * 1 flush pending operation
 * */
static int exit_distributed_process(memory_t* mm_data, int flush)
{
	struct task_struct *g;
	unsigned long flags;
	int is_last_thread_in_local_group = 1;
	int count = 0, i, status;

	lock_task_sighand(current, &flags);
	g = current;
	while_each_thread(current, g) {
		if (g->main == 0 && g->distributed_exit == EXIT_ALIVE) {
			is_last_thread_in_local_group = 0;
			goto find;
		}
	};

find: 
	status = current->distributed_exit;
	current->distributed_exit = EXIT_ALIVE;
	unlock_task_sighand(current, &flags);

	if (mm_data->alive == 0 && !is_last_thread_in_local_group && atomic_read(&(mm_data->pending_migration))==0) {
		printk("%s: ERROR: mm_data->alive is 0 but there are alive threads (id %d, cpu %d)\n", __func__, current->tgroup_home_id, current->tgroup_home_cpu);
		return 0;
	}

	if (mm_data->alive == 0  && atomic_read(&(mm_data->pending_migration))==0) {

		if (status == EXIT_THREAD) {
			printk("%s: ERROR: alive is 0 but status is exit thread (id %d, cpu %d)\n", __func__, current->tgroup_home_id, current->tgroup_home_cpu);
			return flush;
		}

		if (status == EXIT_PROCESS) {
			if (flush == 0) {
				//this is needed to flush the list of pending operation before die
				mm_data->arrived_op = 0;
				vma_server_enqueue_vma_op(mm_data, 0, 1);
				return 1;
			}
		}

		if (flush == 1 && mm_data->arrived_op == 0) {
			if (status == EXIT_FLUSHING)
				printk("%s: ERROR: status exit flush but arrived op is 0 (id %d, cpu %d)\n", __func__, current->tgroup_home_id, current->tgroup_home_cpu);
			return 1;
		}

		if (atomic_read(&(mm_data->pending_migration))!=0)
			printk(KERN_ALERT"%s: ERROR pending migration when cleaning memory (id %d, cpu %d)\n", __func__, current->tgroup_home_id, current->tgroup_home_cpu);

		/*
		 * Empty unused shadow thread pool

		shadow_thread_t* my_shadow = NULL;
		my_shadow = (shadow_thread_t*)pop_data((data_header_t **)&(my_thread_pool->threads),
							 &(my_thread_pool->spinlock));

		while (my_shadow){
			my_shadow->thread->distributed_exit = EXIT_THREAD;
			wake_up_process(my_shadow->thread);
			kfree(my_shadow);
			my_shadow = (shadow_thread_t*)pop_data((data_header_t **)&(my_thread_pool->threads),
								 &(my_thread_pool->spinlock));
		}
		*/
		remove_memory_entry(mm_data);
		mmput(mm_data->mm);
		kfree(mm_data);
#if STATISTICS
		PSPRINTK("%s: page_fault %i fetch %i local_fetch %i write %i read %i most_long_read %i invalid %i ack %i answer_request %i answer_request_void %i request_data %i most_written_page %i concurrent_writes %i most long write %i pages_allocated %i compressed_page_sent %i not_compressed_page %i not_compressed_diff_page %i  (id %d, cpu %d)\n", __func__ , 
			 page_fault_mio,fetch,local_fetch,write,read,most_long_read,invalid,ack,answer_request,answer_request_void, request_data,most_written_page, concurrent_write,most_long_write, pages_allocated,compressed_page_sent,not_compressed_page,not_compressed_diff_page, current->tgroup_home_id, current->tgroup_home_cpu);
#endif
		do_exit(0);

		return 0;
	}


	/* If I am the last thread of my process in this kernel:
	 * - or I am the last thread of the process on all the system => send a group exit to all kernels and erase the mapping saved
	 * - or there are other alive threads in the system => do not erase the saved mapping
	 */
	if (is_last_thread_in_local_group) {
		PSPRINTK("%s: This is the last thread of process (id %d, cpu %d) in the kernel!\n", __func__, current->tgroup_home_id, current->tgroup_home_cpu);
		//mm_data->alive = 0;
		count = count_remote_thread_members(current->tgroup_home_cpu,
							current->tgroup_home_id,mm_data);
		/* Ok this is complicated.
		 * If count is zero=> all the threads of my process went through this exit function (all task->distributed_exit==1 or
		 * there are no more tasks of this process around).
		 * Dying tasks that did not see count==0 saved a copy of the mapping. Someone should notice their kernels that now they can erase it.
		 * I can be the one, however more threads can be concurrently in this exit function on different kernels =>
		 * each one of them can see the count==0 => more than one "erase mapping message" can be sent.
		 * If count==0 I check if I already receive a "erase mapping message" and avoid to send another one.
		 * This check does not guarantee that more than one "erase mapping message" cannot be sent (in some executions it is inevitable) =>
		 * just be sure to not call more than one mmput one the same mapping!!!
		 */
		if (count == 0) {
			mm_data->alive = 0;
			if (status != EXIT_PROCESS) {
				_remote_cpu_info_list_t *objPtr, *tmpPtr;
				thread_group_exited_notification_t *exit_notification = kmalloc(sizeof(*exit_notification),GFP_ATOMIC);

				PSPRINTK("%s: This is the last thread of process (id %d, cpu %d) in the system, "
					 "sending an erase mapping message!\n", __func__, current->tgroup_home_id, current->tgroup_home_cpu);
				exit_notification->header.type = PCN_KMSG_TYPE_PROC_SRV_THREAD_GROUP_EXITED_NOTIFICATION;
				exit_notification->header.prio = PCN_KMSG_PRIO_NORMAL;
				exit_notification->tgroup_home_cpu = current->tgroup_home_cpu;
				exit_notification->tgroup_home_id = current->tgroup_home_id;
				// the list does not include the current processor group descirptor (TODO)
				list_for_each_entry_safe(objPtr, tmpPtr, &rlist_head, cpu_list_member) {
					i = objPtr->_data._processor;
					pcn_kmsg_send_long(i,(struct pcn_kmsg_long_message*)(exit_notification),sizeof(thread_group_exited_notification_t));

				}
				kfree(exit_notification);
			}

			if (flush == 0) {
				//this is needed to flush the list of pending operation before die
				mm_data->arrived_op = 0;
				vma_server_enqueue_vma_op(mm_data, 0, 1);
			} else {
				printk("%s: ERROR: flush is 1 during first exit (alive set to 0 now) (id %d, cpu %d)\n", __func__, current->tgroup_home_id, current->tgroup_home_cpu);
			}
			return 1;
		}
		/*
		 * case i am the last thread but count is not zero
		 * check if there are concurrent migration to be sure if I can put mm_data->alive = 0;
		 */				
		if (atomic_read(&(mm_data->pending_migration))==0)
			mm_data->alive = 0;
	}

	if ((!is_last_thread_in_local_group || count != 0) && status == EXIT_PROCESS) {
		printk("%s: ERROR: received an exit process but is_last_thread_in_local_group id %d and count is %d\n ",
			   __func__, is_last_thread_in_local_group, count);
	}
	return 0;
}

/*
 * Function:
 *		create_shadow_thread
 *
 * Description:
 *		this function creates an empty thread and returns its task
 *		structure
 *
 * Input:
 * 	flags,	the clone flags to be used to create the new thread
 *
 * Output:
 * 	none
 *
 * Return value:
 * 	on success,	returns pointer to newly created task's structure,
 *	on failure, returns NULL
 */
/*
shadow_thread_t *create_shadow_thread(void)
{
	pid_t pid;
	shadow_thread_t *shadow;
	
	shadow = kmalloc(sizeof(*shadow), GFP_KERNEL);
	if (!shadow) return ERR_PTR(-ENOMEM);

	init_completion(&shadow->start);
	init_completion(&shadow->end);
	INIT_LIST_HEAD(&shadow->list);

	pid = kernel_thread_popcorn(shadow_main, shadow);

	if (pid >= 0) {
		PSPRINTK("%s: pid=%d\n", __func__, pid);
		shadow->thread = find_task_by_vpid(pid);
	} else {
		printk("%s: do_fork failed pid=%d\n", __func__, pid);
		kfree(shadow);
		shadow = ERR_PTR(-ESRCH);
	}

	return shadow;
}
*/


///////////////////////////////////////////////////////////////////////////////
// main for distributed kernel thread (?)
///////////////////////////////////////////////////////////////////////////////
#define GET_UNMAP_IF_HOME(task, mm_data) \
({ if (task->tgroup_home_cpu != _cpu) {\
	if (mm_data->mm->distribute_unmap == 0) \
		printk(KERN_ALERT"%s: GET_UNMAP_IF_HOME: ERROR: value was already 0, check who is the older.\n", __func__); \
	mm_data->mm->distribute_unmap = 0; \
	} \
})
#define PUT_UNMAP_IF_HOME(task, mm_data) \
({ if (task->tgroup_home_cpu != _cpu) {\
	if (mm_data->mm->distribute_unmap == 1) \
		printk(KERN_ALERT"%s: GET_UNMAP_IF_HOME: ERROR: value was already 1, check who is the older.\n", __func__); \
	mm_data->mm->distribute_unmap = 1; \
	} \
})

extern long madvise_remove(struct vm_area_struct *vma, struct vm_area_struct **prev, unsigned long start, unsigned long end);
extern int kernel_mprotect(unsigned long start, size_t len, unsigned long prot);
extern long kernel_mremap(unsigned long addr, unsigned long old_len,
		unsigned long new_len, unsigned long flags,
		unsigned long new_addr);

int popcorn_process_exit(long code)
{
	memory_t *memory = current->memory;
	if (!memory) return -ESRCH;

	if (current->main) return 0;	// I'm a helper.

	printk("%s: Stopping 0x%p %d\n", __func__, current, current->pid);
	memory->helper_stop = true;
	put_task_struct(memory->helper);

	return 0;
}

static int helper_thread_main(memory_t* memory)
{
	// TODO: Need to explore how this has to be used
	// TODO: Exit the thread
	might_sleep();

	printk("%s: Started pid=%d\n", __func__, current->pid);
	
	while (!memory->helper_stop) {
		int flush = 0;

		__set_task_state(current, TASK_UNINTERRUPTIBLE);
		schedule_timeout(HZ * 10);
		printk("%s: continues 0x%p\n", __func__, current);
		__set_task_state(current, TASK_RUNNING);
		continue;


		while (current->distributed_exit != EXIT_ALIVE) {
			flush = exit_distributed_process(memory, flush);
			msleep(1000);
		}
		while (memory->operation != VMA_OP_NOP && 
		       memory->mm->thread_op == current) {
			struct file* f;
			unsigned long populate = 0;
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
				if (!vma || (vma->vm_start > memory->addr || vma->vm_end < memory->addr))
					printk("%s: ERROR VMA_OP_MADVISE cannot find VMA addr %lx start %lx end %lx\n",
							__func__, memory->addr, (vma ? vma->vm_start : 0), (vma ? vma->vm_end : 0));
				//write = madvise_need_mmap_write(behavior);
				//if (write)
				//	down_write(&(memory->mm->mmap_sem));
				//else
					down_read(&(memory->mm->mmap_sem));
				GET_UNMAP_IF_HOME(current, memory);
				ret = madvise_remove(vma, &pvma,
						memory->addr, (memory->addr + memory->len) );
				PUT_UNMAP_IF_HOME(current, memory);
				//if (write)
				//	up_write(&(memory->mm->mmap_sem));
				//else
					up_read(&(memory->mm->mmap_sem));
				break; }

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

			case VMA_OP_MAP:
				ret = -1;
				f = NULL;
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

			default:
				break;
			}
			memory->addr = ret;
			memory->operation = VMA_OP_NOP;

            PSPRINTK("%s: wake up %d (cpu %d id %d)\n", __func__,
					memory->waiting_for_main->pid,
					current->tgroup_home_cpu, current->tgroup_home_id);
			wake_up_process(memory->waiting_for_main);
		}

		if (current->distributed_exit != EXIT_ALIVE ||
				(memory->operation != VMA_OP_NOP && memory->mm->thread_op == current)) {
			__set_task_state(current, TASK_RUNNING);
			printk("%s: continue 0x%p\n", __func__, current);
			continue;
		}
		__set_task_state(current, TASK_UNINTERRUPTIBLE);
		schedule_timeout(HZ * 30);
		printk("%s: continues 0x%p\n", __func__, current);
		__set_task_state(current, TASK_RUNNING);
	}
	printk("%s: Exiting from helper_thread_main\n", __func__);
	return 0;
}

#include <popcorn/page_server.h>

///////////////////////////////////////////////////////////////////////////////
// exit group notification
///////////////////////////////////////////////////////////////////////////////
static void process_exit_group_notification(struct work_struct* work)
{
	exit_group_work_t* request_exit = (exit_group_work_t*) work;
	thread_group_exited_notification_t* msg = request_exit->request;
	unsigned long flags;

	memory_t* mm_data = find_memory_entry(msg->tgroup_home_cpu,
					      msg->tgroup_home_id);
	if (mm_data) {
		while (mm_data->helper == NULL)
			schedule();

		lock_task_sighand(mm_data->helper, &flags);
		mm_data->helper->distributed_exit = EXIT_PROCESS;
		unlock_task_sighand(mm_data->helper, &flags);

		wake_up_process(mm_data->helper);
	}

	pcn_kmsg_free_msg(msg);
	kfree(work);
}

static void process_exiting_process_notification(struct work_struct *_work)
{
	exit_work_t* work = (exit_work_t*)_work;
	exiting_process_t* msg = work->request;

	unsigned int source_cpu = msg->header.from_cpu;
	struct task_struct *task;
	
	task = find_task_by_vpid(msg->prev_pid);
	
	if (task && task->next_pid == msg->my_pid
		&& task->next_cpu == source_cpu
	    && task->represents_remote == 1) {

		// TODO: Handle return values
		restore_thread_info(task, &msg->arch);

		task->group_exit = msg->group_exit;
		task->distributed_exit_code = msg->code;
#if MIGRATE_FPU
		// TODO: Handle return values
		restore_fpu_info(task, &msg->arch);
#endif
		wake_up_process(task);
	} else {
		printk(KERN_ALERT"%s: ERROR: task not found."
				"Impossible to kill shadow (pid %d cpu %d)\n",
				__func__, msg->my_pid, source_cpu);
	}

	pcn_kmsg_free_msg(msg);
	kfree(work);
}

static int handle_thread_group_exited_notification(struct pcn_kmsg_message* inc_msg)
{
	exit_group_work_t* work;
	thread_group_exited_notification_t* request =
		(thread_group_exited_notification_t*) inc_msg;

	work = kmalloc(sizeof(exit_group_work_t), GFP_ATOMIC);

	if (work) {
		work->request = request;
		INIT_WORK( (struct work_struct*)work,
			   process_exit_group_notification);
		queue_work(exit_group_wq, (struct work_struct*) work);
	}
	return 1;
}

static int handle_exiting_process_notification(struct pcn_kmsg_message* inc_msg)
{
	exit_work_t* work;
	exiting_process_t* request = (exiting_process_t*) inc_msg;

	work = kmalloc(sizeof(exit_work_t), GFP_ATOMIC);
	if (work) {
		work->request = request;
		INIT_WORK( (struct work_struct*)work,
			   process_exiting_process_notification);
		queue_work(exit_wq, (struct work_struct*)work);
	}
	return 1;
}

///////////////////////////////////////////////////////////////////////////////
// pairing request
///////////////////////////////////////////////////////////////////////////////
/**
 * Handler function for when another processor informs the current cpu
 * of a pid pairing.
 */
static int handle_process_pairing_request(struct pcn_kmsg_message* _msg)
{
	create_process_pairing_t* req = (create_process_pairing_t *)_msg;
	unsigned int source_cpu = req->header.from_cpu;
	struct task_struct* task;
	
	task = find_task_by_vpid(req->your_pid);
	if (task == NULL || task->represents_remote == 0) {
		pcn_kmsg_free_msg(req);
		return -ESRCH;
	}
	task->next_cpu = source_cpu;
	task->next_pid = req->my_pid;
	task->executing_for_remote = 0;

	pcn_kmsg_free_msg(req);
	return 0;
}

static int handle_remote_thread_count_response(struct pcn_kmsg_message* inc_msg)
{
	remote_thread_count_response_t* msg =
			(remote_thread_count_response_t *)inc_msg;
	count_answers_t* data = 
			find_count_entry(msg->tgroup_home_cpu, msg->tgroup_home_id);
	unsigned long flags;
	struct task_struct* to_wake = NULL;

	PSPRINTK("%s: entered - cpu{%d}, id{%d}, count{%d}\n", __func__,
			msg->tgroup_home_cpu, msg->tgroup_home_id, msg->count);

	if (data == NULL) {
		printk("%s: ERROR: unable to find remote thread count data"
				"(cpu %d id %d)\n",
				__func__, msg->tgroup_home_cpu, msg->tgroup_home_id);
		pcn_kmsg_free_msg(inc_msg);
		return -1;
	}

	spin_lock_irqsave(&(data->lock), flags);

	// Register this response.
	data->responses++;
	data->count += msg->count;

	if (data->responses >= data->expected_responses)
		to_wake = data->waiting;

	spin_unlock_irqrestore(&(data->lock), flags);

	if (to_wake != NULL) {
		PSPRINTK("%s: wake up %d\n", __func__, to_wake->pid);
		wake_up_process(to_wake);
	}

	pcn_kmsg_free_msg(inc_msg);
	return 0;
}

static void process_count_request(struct work_struct* _work)
{
	count_work_t* work = (count_work_t*)_work;
	remote_thread_count_request_t* request = work->request;
	remote_thread_count_response_t* response;
	memory_t *memory;

	PSPRINTK("%s: entered - cpu{%d}, id{%d}\n", __func__,
			request->tgroup_home_cpu, request->tgroup_home_id);

	response = kmalloc(sizeof(*response), GFP_ATOMIC);
	BUG_ON(!response);
	response->count = 0;

	/* This is needed to know if the requesting kernel has to save the mapping or send the group dead message.
	 * If there is at least one alive thread of the process in the system the mapping must be saved.
	 * I count how many threads there are but actually I can stop when I know that there is one.
	 * If there are no more threads in the system, a group dead message should be sent by at least one kernel.
	 * I do not need to take the sighand lock (used to set task->distributed_exit=1) because:
	 * --count remote thread is called AFTER set task->distributed_exit=1
	 * --if I am here the last thread of the process in the requesting kernel already set his flag distributed_exit to 1
	 * --two things can happend if the last thread of the process is in this kernel and it is dying too:
	 * --1. set its flag before I check it => I send 0 => the other kernel will send the message
	 * --2. set its flag after I check it => I send 1 => I will send the message
	 * Is important to not take the lock so everything can be done in the messaging layer without fork another kthread.
	 */

	memory = find_memory_entry(request->tgroup_home_cpu, request->tgroup_home_id);
	if (memory != NULL) {
		struct task_struct *t;
		while (memory->helper == NULL)
			schedule();

		t = memory->helper;
		while_each_thread(memory->helper, t) {
			if (t->distributed_exit == EXIT_ALIVE && t->main != 1) {
				response->count++;
				break;
			}
		};
	}

	// Finish constructing response
	response->header.type = PCN_KMSG_TYPE_PROC_SRV_THREAD_COUNT_RESPONSE;
	response->header.prio = PCN_KMSG_PRIO_NORMAL;
	response->tgroup_home_cpu = request->tgroup_home_cpu;
	response->tgroup_home_id = request->tgroup_home_id;
	PSPRINTK(KERN_EMERG"%s: responding to thread count request with %d\n",
			__func__, response->count);
	
	// Send response
	pcn_kmsg_send_long(request->header.from_cpu,
			(struct pcn_kmsg_long_message*)response,
			sizeof(*response));

	pcn_kmsg_free_msg(request);
	kfree(response);
	kfree(work);
}

static int handle_remote_thread_count_request(struct pcn_kmsg_message* inc_msg)
{
	count_work_t* work;
	remote_thread_count_request_t* request = 
			(remote_thread_count_request_t*)inc_msg;

	work = kmalloc(sizeof(*work), GFP_ATOMIC);
	BUG_ON(!work);

	work->request = request;
	INIT_WORK( (struct work_struct*)work, process_count_request);
	queue_work(exit_wq, (struct work_struct*)work);

	return 1;
}

///////////////////////////////////////////////////////////////////////////////
// synch migrations (?)
///////////////////////////////////////////////////////////////////////////////
void synchronize_migrations(int tgroup_home_cpu,int tgroup_home_id )
{
	memory_t* memory = NULL;

	memory = find_memory_entry(tgroup_home_cpu, tgroup_home_id);
	if (!memory) {
		printk("%s: ERROR: no memory_t found (cpu %d id %d)\n",
				__func__, tgroup_home_cpu, tgroup_home_id);
		BUG();
		return;
	}

	/*while (atomic_read(&(memory->pending_back_migration))<57) {
	  msleep(1);
	  }*/
	//int app= atomic_add_return(-1,&(memory->pending_back_migration));
	/*if (app==57)
	  atomic_set(&(memory->pending_back_migration),0);*/
	
	atomic_dec(&(memory->pending_back_migration));
	while (atomic_read(&(memory->pending_back_migration))!=0) {
		msleep(1);
	}
}

///////////////////////////////////////////////////////////////////////////////
// handling back migration
///////////////////////////////////////////////////////////////////////////////
static int handle_back_migration(struct pcn_kmsg_message* inc_msg)
{
	back_migration_request_t* req = (back_migration_request_t*) inc_msg;
	memory_t* memory = NULL;
	struct task_struct *task;

#if MIGRATION_PROFILE
	migration_start = ktime_get();
#endif
	//for synchronizing migratin threads

	PSPRINTK(" IN %s: %d %d\n", __func__, 
			req->tgroup_home_cpu, req->tgroup_home_id);
	memory = find_memory_entry( req->tgroup_home_cpu, req->tgroup_home_id);
	if (memory) {
		atomic_inc(&memory->pending_back_migration);
	} else {
		printk("%s: ERROR: did not find a memory_t struct! (cpu %d id %d)\n",
				__func__, req->tgroup_home_cpu, req->tgroup_home_id);
	}

	//temporary code to check if the back migration can be faster
	task = pid_task(find_get_pid(req->prev_pid), PIDTYPE_PID);

	if (task != NULL &&
			(task->next_pid == req->placeholder_pid) &&
			(task->next_cpu == req->header.from_cpu) &&
			(task->represents_remote == 1)) {
		// TODO: Handle return values
		restore_thread_info(task, &req->arch);
		initialize_thread_retval(task, 0);

		task->prev_cpu = req->header.from_cpu;
		task->prev_pid = req->placeholder_pid;
		task->personality = req->personality;
		task->executing_for_remote = 1;
		task->represents_remote = 0;
		wake_up_process(task);

#if MIGRATION_PROFILE
		migration_end = ktime_get();
  		printk(KERN_ERR"Time for x86->arm back migration - %s side: %ld ns\n",
#if defined(CONFIG_ARM64)
				"ARM",
#else
				"x86",
#endif
				(unsigned long)ktime_to_ns(ktime_sub(migration_end, migration_start)));
#endif
	} else {
		printk("%s: ERROR: task not found. (pid %d cpu %d)\n", __func__,
				req->placeholder_pid, req->header.from_cpu);
	}

	pcn_kmsg_free_msg(req);
	return 0;
}

///////////////////////////////////////////////////////////////////////////////
// exit notification
///////////////////////////////////////////////////////////////////////////////
/**
 * Notify of the fact that either a delegate or placeholder has died locally.
 * In this case, the remote cpu housing its counterpart must be notified, so
 * that it can kill that counterpart.
 */
int process_server_task_exit_notification(struct task_struct *tsk, long code)
{
	int tx_ret = -1;
	int count = 0;
	memory_t* entry = NULL;
	unsigned long flags;
	
	PSPRINTK("MORTEEEEEE-Process_server_task_exit_notification - pid{%d}\n",
			tsk->pid);

	if (tsk->distributed_exit==EXIT_ALIVE) {
		entry = find_memory_entry(tsk->tgroup_home_cpu, tsk->tgroup_home_id);
		if (entry) {
			while (entry->helper == NULL)
				schedule();
		} else {
			printk("%s: ERROR: Mapping disappeared, cannot wake up main thread... (cpu %d id %d)\n",
					__func__, tsk->tgroup_home_cpu, tsk->tgroup_home_id);
			return -1;
		}

		lock_task_sighand(tsk, &flags);
		tsk->distributed_exit = EXIT_THREAD;
		if (entry->helper->distributed_exit == EXIT_ALIVE)
			entry->helper->distributed_exit = EXIT_THREAD;
		unlock_task_sighand(tsk, &flags);

		/* If I am executing on behalf of a thread on another kernel,
		 * notify the shadow of that thread that I am dying.
		 */
		if (tsk->executing_for_remote) {
			exiting_process_t* msg = (exiting_process_t*) kmalloc(
				sizeof(exiting_process_t), GFP_ATOMIC);

			if (msg != NULL) {
				msg->header.type = PCN_KMSG_TYPE_PROC_SRV_EXIT_PROCESS;
				msg->header.prio = PCN_KMSG_PRIO_NORMAL;
				msg->my_pid = tsk->pid;
				msg->prev_pid = tsk->prev_pid;

				// TODO: Handle return value
				save_thread_info(tsk, task_pt_regs(tsk), &msg->arch, NULL);

				if (tsk->group_exit == 1)
					msg->group_exit = 1;
				else
					msg->group_exit = 0;
				msg->code = code;

				msg->is_last_tgroup_member = (count == 1 ? 1 : 0);
#if MIGRATE_FPU
				// TODO: Handle return value
				save_fpu_info(tsk, &msg->arch);
#endif
				//printk("message exit to shadow sent\n");
				tx_ret = pcn_kmsg_send_long(tsk->prev_cpu,
							    (struct pcn_kmsg_long_message*) msg,
							    sizeof(exiting_process_t));
				kfree(msg);
			}
		}
		wake_up_process(entry->helper);
	}
	return tx_ret;
}

/**
 * Create a pairing between a newly created delegate process and the
 * remote placeholder process.  This function creates the local
 * pairing first, then sends a message to the originating cpu
 * so that it can do the same.
 */
static int process_server_notify_delegated_subprocess_starting(
		pid_t my_pid, pid_t remote_pid, int remote_cpu)
{
	create_process_pairing_t* msg;
	int tx_ret = -1;

	msg = kmalloc(sizeof(*msg), GFP_ATOMIC);
	BUG_ON(!msg);

	// Notify remote cpu of pairing between current task and remote
	// representative task.
	msg->header.type = PCN_KMSG_TYPE_PROC_SRV_CREATE_PROCESS_PAIRING;
	msg->header.prio = PCN_KMSG_PRIO_NORMAL;
	msg->your_pid = remote_pid;
	msg->my_pid = my_pid;

	tx_ret = pcn_kmsg_send_long(remote_cpu, msg, sizeof(*msg));

	kfree(msg);

	return tx_ret;
}


///////////////////////////////////////////////////////////////////////////////
// task functions
///////////////////////////////////////////////////////////////////////////////

/*
 * Send a message to <dst_cpu> for migrating back a task <task>.
 * This is a back migration => <task> must already been migrated at least once in <dst_cpu>.
 * It returns -1 in error case.
 */
static int do_back_migration(struct task_struct *task, int dst_cpu,
			     struct pt_regs *regs, void __user *uregs)
{
	back_migration_request_t* req;
	unsigned long flags;
	int ret;

	might_sleep();

	req = kmalloc(sizeof(*req), GFP_KERNEL);
	BUG_ON(req);

	PSPRINTK("%s\n", __func__);

	//printk("%s entered dst{%d}\n",__func__,dst_cpu);
	req->header.type = PCN_KMSG_TYPE_PROC_SRV_BACK_MIG_REQUEST;
	req->header.prio = PCN_KMSG_PRIO_NORMAL;

	req->tgroup_home_cpu = task->tgroup_home_cpu;
	req->tgroup_home_id = task->tgroup_home_id;
	req->placeholder_pid = task->pid;
	req->placeholder_tgid = task->tgid;
	req->prev_pid= task->prev_pid;
	req->personality = task->personality;

	/*mklinux_akshay*/
	req->origin_pid = task->origin_pid;
	req->remote_blocked = task->blocked;
	req->remote_real_blocked = task->real_blocked;
	req->remote_saved_sigmask = task->saved_sigmask;
	req->remote_pending = task->pending;
	req->sas_ss_sp = task->sas_ss_sp;
	req->sas_ss_size = task->sas_ss_size;
	memcpy(req->action, task->sighand->action, sizeof(req->action));

	// TODO: Handle return value
	save_thread_info(task, regs, &req->arch, uregs);

#if MIGRATE_FPU
	// TODO: Handle return value
	save_fpu_info(task, &req->arch);
#endif

	lock_task_sighand(task, &flags);

	if (task->tgroup_distributed == 0) {
		unlock_task_sighand(task, &flags);
		printk("%s: ERROR: not tgroup_distributed process (cpu %d id %d)\n",
				__func__, task->tgroup_home_cpu, task->tgroup_home_id);
		ret = -EINVAL;
		goto out_free;
	}

	task->represents_remote = 1;
	task->next_cpu = task->prev_cpu;
	task->next_pid = task->prev_pid;
	task->executing_for_remote = 0;
	unlock_task_sighand(task, &flags);

#if MIGRATION_PROFILE
	migration_end = ktime_get();
	printk(KERN_ERR "Time for x86->arm back migration - x86 side: %ld ns\n",
			(unsigned long)ktime_to_ns(ktime_sub(migration_end, migration_start)));
#endif
	ret = pcn_kmsg_send_long(dst_cpu, req, sizeof(*req));

out_free:
	kfree(req);
	return ret;
}

/*
 * Send a message to <dst_cpu> for migrating a task <task>.
 * This function will ask the remote cpu to create a thread to host task.
 * It returns -1 in error case.
 */
//static
static int handle_clone_request(struct pcn_kmsg_message* inc_msg);

static int create_helper_thread_from_user(void *_memory)
{
	memory_t *memory = (memory_t *)_memory;

	might_sleep();

	BUG_ON(memory != current->memory);
	BUG_ON(memory->helper != current);

	current->main = 1;	// Yes, i'm a helper

	helper_thread_main(memory);

	return 0;
}

int do_migration(struct task_struct *task, int dst_cpu,
		struct pt_regs *regs, void __user *uregs)
{
	int sent = -1;
	struct task_struct* tgroup_iterator = NULL;
	char* rpath, path[512];
	memory_t* memory;
	bool create_helper = false;
	unsigned long flags;
	clone_request_t* req;

	req = kmalloc(sizeof(*req), GFP_KERNEL);
	if (req == NULL) return -ENOMEM;

	/* Build request */
	req->header.type = PCN_KMSG_TYPE_PROC_SRV_CLONE_REQUEST;
	req->header.prio = PCN_KMSG_PRIO_NORMAL;

	// struct mm_struct --------------------------------------------------------
	rpath = d_path(&task->mm->exe_file->f_path, path, sizeof(path));
	if (IS_ERR(rpath)) {
		printk("%s: exe binary path is too long.\n", __func__);
		kfree(req);
		return -ESRCH;
	} else {
		strncpy(req->exe_path, rpath, sizeof(req->exe_path));
		printk("%s: exe_path=%s\n", __func__, rpath);
	}

	req->stack_start = task->mm->start_stack;
	req->start_brk = task->mm->start_brk;
	req->brk = task->mm->brk;
	req->env_start = task->mm->env_start;
	req->env_end = task->mm->env_end;
	req->arg_start = task->mm->arg_start;
	req->arg_end = task->mm->arg_end;
	req->start_code = task->mm->start_code;
	req->end_code = task->mm->end_code;
	req->start_data = task->mm->start_data;
	req->end_data = task->mm->end_data;
	req->def_flags = task->mm->def_flags;
	req->popcorn_vdso = task->mm->context.popcorn_vdso;
	
	// struct task_struct ------------------------------------------------------
	req->placeholder_pid = task->pid;
	req->placeholder_tgid = task->tgid;

	/*mklinux_akshay*/
	if (task->prev_pid == -1)
		req->origin_pid = task->pid;
	else
		req->origin_pid = task->origin_pid;
	req->remote_blocked = task->blocked;
	req->remote_real_blocked = task->real_blocked;
	req->remote_saved_sigmask = task->saved_sigmask;
	req->remote_pending = task->pending;
	req->sas_ss_sp = task->sas_ss_sp;
	req->sas_ss_size = task->sas_ss_size;

	/*mklinux_akshay*/
	if (task->prev_pid == -1)
		task->origin_pid = task->pid;
	else
		task->origin_pid = task->origin_pid;

	req->personality = task->personality;

	// TODO: Handle return value
	if (save_thread_info(task, regs, &req->arch, uregs) != 0) {
		kfree(req);
		return -EINVAL;
	}

#if MIGRATE_FPU
	save_fpu_info(task, &req->arch);
#endif

	/* I use siglock to coordinate the thread group.
	 * This process is becoming a distributed one if it was not already.
	 * The first migrating thread has to create the memory entry to 
	 * handle page requests, and fork the main kernel thread of this process.
	 */
	lock_task_sighand(task, &flags);

	// convert the task to distributed one
	if (task->tgroup_distributed == 0) {
		create_helper = true;
		task->tgroup_distributed = 1;

		task->tgroup_home_id = task->tgid;
		task->tgroup_home_cpu = _cpu;

		memory = kmalloc(sizeof(memory_t), GFP_ATOMIC);
		BUG_ON(!memory);

		INIT_LIST_HEAD(&memory->list);
		memory->mm = task->mm;
		atomic_inc(&memory->mm->mm_users);
		memory->tgroup_home_cpu = task->tgroup_home_cpu;
		memory->tgroup_home_id = task->tgroup_home_id;
		memory->alive = 1;
		memory->helper = NULL;
		atomic_set(&memory->pending_migration, 0);
		atomic_set(&memory->pending_back_migration, 0);
		memory->operation = VMA_OP_NOP;
		memory->waiting_for_main = NULL;
		memory->waiting_for_op = NULL;
		memory->arrived_op = 0;
		memory->my_lock = 0;
		memset(memory->kernel_set, 0, sizeof(memory->kernel_set));
		memory->kernel_set[_cpu]= 1;
		init_rwsem(&memory->kernel_set_sem);

		add_memory_entry(memory);
		task->memory = memory;

		init_rwsem(&task->mm->distribute_sem);
		task->mm->distr_vma_op_counter = 0;
		task->mm->was_not_pushed = 0;
		task->mm->thread_op = NULL;
		task->mm->vma_operation_index = 0;
		task->mm->distribute_unmap = 1;

		tgroup_iterator = task;
		while_each_thread(task, tgroup_iterator) {
			tgroup_iterator->tgroup_home_id = task->tgroup_home_id;
			tgroup_iterator->tgroup_home_cpu = task->tgroup_home_cpu;
			tgroup_iterator->tgroup_distributed = 1;
		};

		printk("%s: task distributed. cpu=%d, id=%d\n", __func__,
				task->tgroup_home_cpu, task->tgroup_home_id);
	}

	task->represents_remote = 1;

	memcpy(req->action, task->sighand->action, sizeof(req->action));

	req->tgroup_home_cpu = task->tgroup_home_cpu;
	req->tgroup_home_id = task->tgroup_home_id;

	unlock_task_sighand(task, &flags);

#if MIGRATION_PROFILE
	migration_end = ktime_get();
	printk(KERN_ERR"Time for x86->arm migration - x86 side: %ld ns\n",
		(unsigned long)ktime_to_ns(ktime_sub(migration_end,migration_start)));
#endif

	if (create_helper) {
		pid_t pid = kernel_thread_popcorn(create_helper_thread_from_user, memory);
		memory->helper = find_task_by_vpid(pid);
		memory->helper_stop = false;
		get_task_struct(memory->helper);
	}
	/*
	sent  = pcn_kmsg_send_long(dst_cpu, req, sizeof(*req));
	kfree(req);
	*/
	sent = handle_clone_request((void *)req);
	sent = 0;

	return sent;
}

/**
 * Migrate the specified task <task> to cpu <dst_cpu>
 * Currently, this function will put the specified task to
 * sleep, and push its info over to the remote cpu.  The
 * remote cpu will then create a new thread and import that
 * info into its new context.
 *
 * It returns PROCESS_SERVER_CLONE_FAIL in case of error,
 * PROCESS_SERVER_CLONE_SUCCESS otherwise.
 */
int process_server_do_migration(struct task_struct *task, int dst_cpu,
				struct pt_regs *regs, void __user *uregs)
{
	int ret = 0;

	PSPRINTK(KERN_INFO"%s: current=pid %d tgid %d "
			"task->tgroup_home_id %d task->tgroup_home_cpu %d\n",
			__func__, current->pid, current->tgid,
			task->tgroup_home_id, task->tgroup_home_cpu);

	if (task->prev_cpu == dst_cpu) {
		ret = do_back_migration(task, dst_cpu, regs, uregs);
	} else {
		ret = do_migration(task, dst_cpu, regs, uregs);
	}

	if (ret < 0) {
		return PROCESS_SERVER_CLONE_FAIL;
	}

	//printk(KERN_ALERT"%s clone request sent ret{%d}\n", __func__,ret);
	//__set_task_state(task, TASK_UNINTERRUPTIBLE);
	return PROCESS_SERVER_CLONE_SUCCESS;
}

int shadow_main(void *_args)
{
	memory_t* memory = NULL;
	struct completion *start = (struct completion *)_args;
    PSPRINTK("%s: start pid=%d, tsk=0x%p, mm=0x%p\n", __func__,
			current->pid, current, current->mm);

	wait_for_completion(start);

	// ---------------- Attached ---------------------

	PSPRINTK("%s: resume 0x%p %d\n", __func__, current, current->pid);
	if (current->distributed_exit != EXIT_NOT_ACTIVE) {
		current->represents_remote = 0;
		return 0;
	}

	current->distributed_exit = EXIT_ALIVE;
	current->represents_remote = 0;

	// Notify of PID/PID pairing.
	process_server_notify_delegated_subprocess_starting(
			current->pid, current->prev_pid, current->prev_cpu);
	printk("%s: OK 1\n", __func__);

	//this force the task to wait that the main correctly set up the memory
	while (current->tgroup_distributed != 1) {
		printk("%s: OK 2\n", __func__);
		msleep(1);
		printk("%s: OK 3\n", __func__);
	}

	PSPRINTK("%s main set up pid %d\n", __func__, current->pid);
	memory = find_memory_entry(
			current->tgroup_home_cpu, current->tgroup_home_id);
	if (!memory) {
		printk("%s: WARN: cannot find memory_t (cpu %d id %d)\n",
				__func__, current->tgroup_home_cpu, current->tgroup_home_id);
	} else
		memory->alive = 1;

	// TODO: Handle return values
	/* beowulf
	update_thread_info(current);

	//to synchronize migrations...
	atomic_dec(&(memory->pending_migration));

	//to synchronize migrations...
	while (atomic_read(&(memory->pending_migration)) != 0) {
		msleep(1);
	}

#if MIGRATE_FPU
	// TODO: Handle return values
	update_fpu_info(current);
#endif
	*/
	PSPRINTK("%s sleep pid %d\n", __func__, current->pid);
	msleep(5000);
	PSPRINTK("%s return pid %d\n", __func__, current->pid);

	do_exit(0);
}


static int go_exit = 1;
int shadow_main_(void *_args)
{
	struct completion *start = (struct completion *)_args;
	int i;

    PSPRINTK("%s: start pid=%d, tsk=0x%p, mm=0x%p\n", __func__,
			current->pid, current, current->mm);

	for (i = 0; i < 2; i++) {
		msleep(100);
		printk("%s: + 0x%p %d\n", __func__, current, i);
	}

	while (go_exit) {
		printk("%s: loop!\n", __func__);
		set_task_state(current, TASK_INTERRUPTIBLE);
		complete(start);
		schedule_timeout(HZ * 1);
		set_task_state(current, TASK_RUNNING);
	}
	update_thread_info(current);

	for (i = 0; i < 4; i++) {
		msleep(500);
		printk("%s: - 0x%p %d\n", __func__, current, i);
	}
}

///////////////////////////////////////////////////////////////////////////////
// thread specific handling
///////////////////////////////////////////////////////////////////////////////
static int create_user_thread_for_distributed_process(clone_request_t* req)
{
	struct task_struct *task;
	struct completion *start = kmalloc(sizeof(*start), GFP_KERNEL);
	//int ret;
	const unsigned long flags = CLONE_THREAD | CLONE_SIGHAND | 
						  CLONE_VM | CLONE_UNTRACED | SIGCHLD;
	pid_t pid;
	go_exit = 1;

	BUG_ON(!start);
	init_completion(start);
	pid = kernel_thread(shadow_main_, start, flags);
	BUG_ON(pid < 0);
	task = find_task_by_vpid(pid);

	wait_for_completion(start);

	printk(KERN_INFO"%s: %d@0x%p for %d@0x%p\n", __func__,
			pid, task, current->pid, current);
	__rename_task_comm(task, req->exe_path);

	/* if we are already attached, let's skip the unlinking and linking */
	//associate the task with the namespace
	/*
	if (task->nsproxy->cpu_ns != popcorn_ns) {
		// TODO temp fix or of all active cpus?!
		// ---- TODO this must be fixed is not acceptable
		do_set_cpus_allowed(task, cpu_online_mask);
		put_cpu_ns(task->nsproxy->cpu_ns);
		task->nsproxy->cpu_ns = get_cpu_ns(popcorn_ns);
	}
	*/
	/*
	ret = associate_to_popcorn_ns(task);
	if (ret) {
		printk(KERN_ERR "%s: ERROR: associate_to_popcorn_ns returned: %d\n",
				__func__,ret);
	}
	*/

	// set thread info
	// TODO: Handle return values
	restore_thread_info(task, &req->arch);
	initialize_thread_retval(task, 0);

	task->prev_cpu = req->header.from_cpu;
	task->prev_pid = req->placeholder_pid;
	task->personality = req->personality;
	//	task->origin_pid = req->origin_pid;
	//	sigorsets(&task->blocked,&task->blocked,&req->remote_blocked) ;
	//	sigorsets(&task->real_blocked,&task->real_blocked,&req->remote_real_blocked);
	//	sigorsets(&task->saved_sigmask,&task->saved_sigmask,&req->remote_saved_sigmask);
	//	task->pending = req->remote_pending;
	//	task->sas_ss_sp = req->sas_ss_sp;
	//	task->sas_ss_size = req->sas_ss_size;
	//	for (int cnt = 0; cnt < _NSIG; cnt++)
	//		task->sighand->action[cnt] = req->action[cnt];
#if MIGRATE_FPU
	restore_fpu_info(task, &req->arch);
#endif
	//the task will be activated only when task->executing_for_remote==1
	task->executing_for_remote = 1;
	PSPRINTK("%s: wake up %d\n", __func__, task->pid);
	wake_up_process(task);

#if MIGRATION_PROFILE
	migration_end = ktime_get();
	printk(KERN_ERR "Time for arm->x86 migration - x86 side: %ld ns\n",
			ktime_to_ns(ktime_sub(migration_end,migration_start)));
#endif

	printk("####### MIGRATED - pid %u@%d to %u@%d\n",
			(unsigned int)task->prev_pid, task->prev_cpu, task->pid, _cpu);
	printk("%s: pc %lx\n", __func__, (&req->arch)->migration_pc);
	printk("%s: ip %lx\n", __func__, task_pt_regs(task)->ip);
	printk("%s: sp %lx\n", __func__, task_pt_regs(task)->sp);
	printk("%s: bp %lx\n", __func__, task_pt_regs(task)->bp);

	return 0;
}


/* Construct mm_struct */
static int __construct_helper_mm(clone_request_t *req)
{
	struct mm_struct *mm;
	struct file* f;

	mm = mm_alloc();
	BUG_ON(!mm);
	
	arch_pick_mmap_layout(mm);
	// atomic_inc to prevent mm from being released during exec_mmap
	atomic_inc(&mm->mm_users);
	exec_mmap(mm);
	atomic_dec(&mm->mm_users);
	set_fs(USER_DS);

	f = filp_open(req->exe_path, O_RDONLY | O_LARGEFILE | O_EXCL, 0);
	if (IS_ERR(f)) {
		printk("%s: ERROR to open exe_path %s\n", __func__, req->exe_path);
		return -EINVAL;
	}
	set_mm_exe_file(mm, f);
	filp_close(f, NULL);

	mm->start_stack = req->stack_start;
	mm->start_brk = req->start_brk;
	mm->brk = req->brk;
	mm->env_start = req->env_start;
	mm->env_end = req->env_end;
	mm->arg_start = req->arg_start;
	mm->arg_end = req->arg_end;
	mm->start_code = req->start_code;
	mm->end_code = req->end_code;
	mm->start_data = req->start_data;
	mm->end_data = req->end_data;
	mm->def_flags = req->def_flags;

	init_rwsem(&mm->distribute_sem);
	mm->distr_vma_op_counter = 0;
	mm->was_not_pushed = 0;
	mm->thread_op = NULL;
	mm->vma_operation_index = 0;
	mm->distribute_unmap = 1;

	return 0;
}


static int helper_thread_main_(memory_t *memory)
{
	int count = 10;
	might_sleep();

	printk("%s: Started pid=%d\n", __func__, current->pid);
	
	while (!memory->helper_stop && count > 0) {
		__set_task_state(current, TASK_UNINTERRUPTIBLE);
		schedule_timeout(HZ * 1);
		printk("%s: continues 0x%p %d\n", __func__, current, count--);
		__set_task_state(current, TASK_RUNNING);
	}
	go_exit = 0;
	printk("%s: Exit helper %d\n", __func__, current->pid);

	return 0;
}

/**
 * Remote side kernel thread
 */
static int create_helper_thread(void *_data)
{
	clone_request_t *req = (clone_request_t *)_data;
	struct task_struct* tgroup_iterator = NULL;
	memory_t* memory;
	int ret;
	
	might_sleep();

	PSPRINTK("%s: task=0x%p, pid=%d\n", __func__, current, current->pid);
	PSPRINTK("%s: exe_path=%s\n", __func__, req->exe_path);
	PSPRINTK("%s: cpu=%d, id=%d, prev_pid=%d, origin_pid=%d\n", __func__,
			req->tgroup_home_cpu, req->tgroup_home_id,
			req->prev_pid, req->origin_pid);

	current->tgroup_home_cpu = req->tgroup_home_cpu;
	current->tgroup_home_id = req->tgroup_home_id;
	current->tgroup_distributed = 1;
	current->main = 1;
	current->flags &= ~(PF_RANDOMIZE);

	__rename_task_comm(current, req->exe_path);

	if (__construct_helper_mm(req) != 0) {
		BUG();
		return -EINVAL;
	}

	/* sanghoon: I think there is no point to deal with user-level stuff for
	 * kernel thread...
	struct cred * new;

	spin_lock_irq(&current->sighand->siglock);
	flush_signal_handlers(current, 1);
	spin_unlock_irq(&current->sighand->siglock);

	set_cpus_allowed_ptr(current, cpu_all_mask);
	set_user_nice(current, 0);
	new = prepare_kernel_cred(current);
	commit_creds(new);

	mm = mm_alloc();
	BUG_ON(!mm);
	init_new_context(current, mm);
	arch_pick_mmap_layout(mm);
	exec_mmap(mm);

	set_fs(USER_DS);
	current->flags &= ~(PF_RANDOMIZE | PF_KTHREAD);
	flush_thread();
	flush_signal_handlers(current, 0);

	init_rwsem(&current->mm->distribute_sem);
	current->mm->distr_vma_op_counter = 0;
	current->mm->was_not_pushed = 0;
	current->mm->thread_op = NULL;
	current->mm->vma_operation_index = 0;
	current->mm->distribute_unmap = 1;
	*/

	/*
retry:
	memory = find_memory_entry(req->tgroup_home_cpu, req->tgroup_home_id);
	if (memory) {
		PSPRINTK("%s: memory found\n", __func__);
		atomic_inc(&memory->pending_migration);
		goto out_create_user;
	}
	PSPRINTK("%s: memory not found\n", __func__);
	*/
	memory = kzalloc(sizeof(*memory), GFP_KERNEL);
	BUG_ON(!memory);

	INIT_LIST_HEAD(&memory->list);
	memory->helper = current;
	memory->mm = current->mm;
	memory->tgroup_home_cpu = current->tgroup_home_cpu;
	memory->tgroup_home_id = current->tgroup_home_id;

	memory->operation = VMA_OP_NOP;
	memory->waiting_for_main = NULL;
	memory->waiting_for_op = NULL;
	memory->arrived_op = 0;
	memory->my_lock = 0;
	atomic_set(&memory->pending_migration, 1);
	atomic_set(&memory->pending_back_migration, 0);

	init_rwsem(&memory->kernel_set_sem);
	memset(memory->kernel_set, 0, sizeof(memory->kernel_set));
	memory->kernel_set[_cpu] = 1;

	memory->helper = current;
	memory->helper_stop = false;

	/*
	int ret = add_memory_entry_with_check(memory);
	WARN_ON(ret != 0, "memory entry exists. possible race condition..");
	*/

	// __inform_process_migration()

	tgroup_iterator = current;
	while_each_thread(current, tgroup_iterator) {
		tgroup_iterator->tgroup_home_id = current->tgroup_home_id;
		tgroup_iterator->tgroup_home_cpu = current->tgroup_home_cpu;
		tgroup_iterator->tgroup_distributed = 1;
	};

//out_create_user:
	ret = create_user_thread_for_distributed_process(req);
	pcn_kmsg_free_msg(req);

	memory->alive = 1;

	return helper_thread_main_(memory);
}

/*
 * inform all kernel that a new distributed process is present here
 * the list does not include the current processor group descirptor (TODO)
 */
/*
int inform_process_migration()
{
	new_kernel_t* new_kernel_msg;
	_remote_cpu_info_list_t *r;
	int i;

	new_kernel_msg = kmalloc(sizeof(*new_kernel_msg), GFP_ATOMIC);
	BUG_ON(!new_kernel_msg);

	new_kernel_msg->tgroup_home_cpu = current->tgroup_home_cpu;
	new_kernel_msg->tgroup_home_id = current->tgroup_home_id;

	new_kernel_msg->header.type = PCN_KMSG_TYPE_PROC_SRV_NEW_KERNEL;
	new_kernel_msg->header.prio = PCN_KMSG_PRIO_NORMAL;

	atomic_set(&entry->answers_remain, 0);
	spin_lock_init(&(entry->lock_for_answer));
	
	list_for_each_entry(r, &rlist_head, cpu_list_member) {
		i = r->_data._processor;
		PSPRINTK("sending new kernel message to %d\n", i);
		PSPRINTK("cpu %d id %d\n", current->tgroup_home_cpu, current->tgroup_home_id);

		if (pcn_kmsg_send_long(i, new_kernel_msg, sizeof(new_kernel_t)) != -1) {
			// Message delivered
			atomic_inc(&entry->answers_remain);
		}
	}
	PSPRINTK("%s: sent %d new kernel messages, current %d\n", __func__,
			atomic_read(&entry->answers_remain), current->pid);

	while (atomic_read(&entry->answers_remain) > 0) {
		set_task_state(current, TASK_UNINTERRUPTIBLE);
		schedule();
		set_task_state(current, TASK_RUNNING);
	}

	PSPRINTK("%s: received all answers\n", __func__);
	kfree(new_kernel_msg);

	return 0;
}
*/


///////////////////////////////////////////////////////////////////////////////
// handle clone
///////////////////////////////////////////////////////////////////////////////

static void clone_remote_thread(struct work_struct *_work)
{
	clone_work_t *work = (clone_work_t *)_work;
	clone_request_t* req = (clone_request_t *)work->request;
	struct task_struct *tsk =
			kthread_run(create_helper_thread, req, "helper_%d", 0);
	BUG_ON(IS_ERR(tsk));

	kfree(work);
	return;
}


static int handle_clone_request(struct pcn_kmsg_message* msg)
{
	clone_request_t *req = (clone_request_t*)msg;
	clone_work_t *work = kmalloc(sizeof(*work), GFP_ATOMIC);

#if MIGRATION_PROFILE
	migration_start = ktime_get();
#endif

	BUG_ON(!work);
	work->request = req;
	INIT_WORK((struct work_struct *)work, clone_remote_thread);
	queue_work(clone_wq, (struct work_struct *)work);

	return 0;
}


///////////////////////////////////////////////////////////////////////////////
// init functions
///////////////////////////////////////////////////////////////////////////////

static void __init file_handler_init(void)
{
#if 0 // beowulf
	/*
	 * Register handlers for remote file
	 */
	_file_cpu = _cpu;

	file_wait_q();

	pcn_kmsg_register_callback(PCN_KMSG_TYPE_FILE_OPEN_REQUEST,
					handle_file_open_request);
	pcn_kmsg_register_callback(PCN_KMSG_TYPE_FILE_OPEN_REPLY,
					handle_file_open_reply);
	pcn_kmsg_register_callback(PCN_KMSG_TYPE_FILE_STATUS_REQUEST,
					handle_file_status_request);
	pcn_kmsg_register_callback(PCN_KMSG_TYPE_FILE_STATUS_REPLY,
					handle_file_status_reply);
	pcn_kmsg_register_callback(PCN_KMSG_TYPE_FILE_OFFSET_REQUEST,
					handle_file_offset_request);
	pcn_kmsg_register_callback(PCN_KMSG_TYPE_FILE_OFFSET_REPLY,
					handle_file_offset_reply);
	pcn_kmsg_register_callback(PCN_KMSG_TYPE_FILE_OFFSET_UPDATE,
					handle_file_pos_update);
	pcn_kmsg_register_callback(PCN_KMSG_TYPE_FILE_OFFSET_CONFIRM,
					handle_file_pos_confirm);
#endif
}


/**
 * Initialize the process server.
 */
int __init process_server_init(void)
{
	/*
	 * Create work queues so that we can do bottom side
	 * processing on data that was brought in by the
	 * communications module interrupt handlers.
	 */
	clone_wq = create_workqueue("clone_wq");
	exit_wq  = create_workqueue("exit_wq");
	exit_group_wq = create_workqueue("exit_group_wq");
	new_kernel_wq = create_workqueue("new_kernel_wq");

#if STATISTICS
	/* TODO: make sure to refactor the following */
	page_fault_mio = 0;

	fetch = 0;
	local_fetch = 0;
	write = 0;
	concurrent_write =  0;
	most_long_write = 0;
	read = 0;

	invalid = 0;
	ack = 0;
	answer_request = 0;
	answer_request_void = 0;
	request_data = 0;

	most_written_page =  0;
	most_long_read =  0;
	pages_allocated = 0;

	compressed_page_sent = 0;
	not_compressed_page = 0;
	not_compressed_diff_page = 0;
#endif

	/* Register handlers */
	pcn_kmsg_register_callback(PCN_KMSG_TYPE_PROC_SRV_NEW_KERNEL,
				   handle_new_kernel);
	pcn_kmsg_register_callback(PCN_KMSG_TYPE_PROC_SRV_NEW_KERNEL_ANSWER,
				   handle_new_kernel_answer);

	pcn_kmsg_register_callback(PCN_KMSG_TYPE_PROC_SRV_CLONE_REQUEST,
				   handle_clone_request);
	pcn_kmsg_register_callback(PCN_KMSG_TYPE_PROC_SRV_BACK_MIG_REQUEST,
				   handle_back_migration);
	pcn_kmsg_register_callback(PCN_KMSG_TYPE_PROC_SRV_CREATE_PROCESS_PAIRING,
				   handle_process_pairing_request);

	pcn_kmsg_register_callback(PCN_KMSG_TYPE_PROC_SRV_THREAD_COUNT_REQUEST,
				   handle_remote_thread_count_request);
	pcn_kmsg_register_callback(PCN_KMSG_TYPE_PROC_SRV_THREAD_COUNT_RESPONSE,
				   handle_remote_thread_count_response);
	pcn_kmsg_register_callback(PCN_KMSG_TYPE_PROC_SRV_EXIT_PROCESS,
				   handle_exiting_process_notification);
	pcn_kmsg_register_callback(PCN_KMSG_TYPE_PROC_SRV_THREAD_GROUP_EXITED_NOTIFICATION,
				   handle_thread_group_exited_notification);

	file_handler_init();

	return 0;
}
