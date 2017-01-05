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

//#include <linux/mcomm.h> // IPC
#include <linux/delay.h>
#include <linux/sched.h>
#include <linux/threads.h> // NR_CPUS
#include <linux/slab.h>

#include <asm/mmu_context.h>

#define  NSIG 32

#include <linux/cpu_namespace.h>
#include <linux/popcorn_cpuinfo.h>
#include <linux/process_server.h>

#include <process_server_arch.h>
#include "stat.h"
#include "internal.h"
/*
#include <popcorn/process_server.h>
#include "vma_server.h"
#include <popcorn/vma_server.h>
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

int _cpu = -1;		// This is for any Popcorn Linux server

#if 0 // beowulf
int _file_cpu = 1;	// This is for the Popcorn Linux file server
#endif

///////////////////////////////////////////////////////////////////////////////
// Marina's data stores (linked lists)
///////////////////////////////////////////////////////////////////////////////

mapping_answers_for_2_kernels_t* _mapping_head = NULL;
DEFINE_RAW_SPINLOCK(_mapping_head_lock);

ack_answers_for_2_kernels_t* _ack_head = NULL;
DEFINE_RAW_SPINLOCK(_ack_head_lock);

LIST_HEAD(_memory_head);
DEFINE_SPINLOCK(_memory_head_lock);

LIST_HEAD(_count_head);
DEFINE_SPINLOCK(_count_head_lock);

LIST_HEAD(_vma_ack_head);
DEFINE_SPINLOCK(_vma_ack_head_lock);


thread_pull_t* thread_pull_head = NULL;
DEFINE_RAW_SPINLOCK(thread_pull_head_lock);


static void __copy_task_comm(struct task_struct *task, char *name)
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


///////////////////////////////////////////////////////////////////////////////
// Working queues (servers)
///////////////////////////////////////////////////////////////////////////////
static struct workqueue_struct *clone_wq;
static struct workqueue_struct *exit_wq;
static struct workqueue_struct *exit_group_wq;
static struct workqueue_struct *new_kernel_wq;


static int count_remote_thread_members(int tgroup_home_cpu, int tgroup_home_id, memory_t* mm_data)
{
	count_answers_t* data;
	remote_thread_count_request_t* request;
	int i, s;
	int ret = -1;
	unsigned long flags;
	_remote_cpu_info_list_t *objPtr, *tmpPtr;

	data = kmalloc(sizeof(*data), GFP_ATOMIC);
	BUG_ON(!data);

	data->responses = 0;
	data->tgroup_home_cpu = tgroup_home_cpu;
	data->tgroup_home_id = tgroup_home_id;
	data->count = 0;
	data->waiting = current;
	raw_spin_lock_init(&(data->lock));

	add_count_entry(data);

	request = kmalloc(sizeof(*request),GFP_ATOMIC);
	BUG_ON(!request);

	request->header.type = PCN_KMSG_TYPE_PROC_SRV_THREAD_COUNT_REQUEST;
	request->header.prio = PCN_KMSG_PRIO_NORMAL;
	request->tgroup_home_cpu = tgroup_home_cpu;
	request->tgroup_home_id = tgroup_home_id;

	data->expected_responses = 0;

	down_read(&mm_data->kernel_set_sem);

	//printk("%s before sending data->expected_responses %d data->responses %d\n",__func__,data->expected_responses,data->responses);
	// the list does not include the current processor group descirptor (TODO)

	list_for_each_entry_safe(objPtr, tmpPtr, &rlist_head, cpu_list_member) {
		i = objPtr->_data._processor;

		if (mm_data->kernel_set[i]==1){
			// Send the request to this cpu.
			//s = pcn_kmsg_send(i, (struct pcn_kmsg_message*) (&request));
			s = pcn_kmsg_send_long(i, (struct pcn_kmsg_long_message*)(request),
					sizeof(remote_thread_count_request_t));
			if (s!=-1) {
				// A successful send operation, increase the number
				// of expected responses.
				data->expected_responses++;
			}
		}
	}

	up_read(&mm_data->kernel_set_sem);
	//printk("%s going to sleep data->expected_responses %d data->responses %d\n",__func__,data->expected_responses,data->responses);
	while (data->expected_responses != data->responses) {
		set_task_state(current, TASK_UNINTERRUPTIBLE);
		if (data->expected_responses != data->responses)
			schedule();

		set_task_state(current, TASK_RUNNING);
	}

	//printk("%s waked up data->expected_responses%d data->responses%d\n",__func__,data->expected_responses,data->responses);
	raw_spin_lock_irqsave(&(data->lock), flags);
	raw_spin_unlock_irqrestore(&(data->lock), flags);
	// OK, all responses are in, we can proceed.
	//printk("%s data->count is %d",__func__,data->count);
	ret = data->count;
	remove_count_entry(data);
	kfree(data);
	kfree(request);
	return ret;
}

static void process_new_kernel_answer(struct work_struct* work)
{
	int i;
	new_kernel_work_answer_t* my_work = (new_kernel_work_answer_t*)work;
	new_kernel_answer_t* answer = my_work->answer;
	memory_t* memory = my_work->memory;

	if (answer->header.from_cpu == answer->tgroup_home_cpu) {
		down_write(&memory->mm->mmap_sem);
		//	printk("%s answer->vma_operation_index %d NR_CPU %d\n",__func__,answer->vma_operation_index,MAX_KERNEL_IDS);
		memory->mm->vma_operation_index = answer->vma_operation_index;
		up_write(&memory->mm->mmap_sem);
	}

	down_write(&memory->kernel_set_sem);
	for (i=0;i<MAX_KERNEL_IDS;i++){
		if (answer->my_set[i] == 1)
			memory->kernel_set[i] = 1;
	}
	memory->answers++;

	if (memory->answers >= memory->exp_answ) {
		PSPRINTK("%s: wake up %d (%s)\n", __func__, memory->main->pid, memory->main->comm);
		wake_up_process(memory->main);
	}

#if 0 // beowulf BUG??
	if (memory->answers >= memory->exp_answ) {
		PSPRINTK("%s: wake up %d (%s)\n", __func__, memory->main->pid, memory->main->comm);
		wake_up_process(memory->main);
	}
#endif

	up_write(&memory->kernel_set_sem);

	pcn_kmsg_free_msg(answer);
	kfree(work);
}

///////////////////////////////////////////////////////////////////////////////
// handling a new incoming migration request?
///////////////////////////////////////////////////////////////////////////////
static int handle_new_kernel_answer(struct pcn_kmsg_message* inc_msg)
{
	new_kernel_answer_t* answer= (new_kernel_answer_t*)inc_msg;
	memory_t* memory = find_memory_entry(answer->tgroup_home_cpu,
					    answer->tgroup_home_id);

	PSNEWTHREADPRINTK("received new kernel answer\n");
	//printk("%s: %d\n",__func__,answer->vma_operation_index);
	if (memory != NULL) {
		new_kernel_work_answer_t* work = kmalloc(sizeof(*work), GFP_ATOMIC);
		if (work!=NULL) {
			work->answer = answer;
			work->memory = memory;
			INIT_WORK((struct work_struct*)work, process_new_kernel_answer);
			queue_work(new_kernel_wq, (struct work_struct*)work);
		} else {
			pcn_kmsg_free_msg(inc_msg);
		}
	} else {
		printk("%s: ERROR: received an answer new kernel but memory_t not present cpu %d id %d\n",
				__func__, answer->tgroup_home_cpu, answer->tgroup_home_id);
		pcn_kmsg_free_msg(inc_msg);
	}
	
	return 1;
}

static void process_new_kernel(struct work_struct* work)
{
	new_kernel_work_t* new_kernel_work= (new_kernel_work_t*) work;
	memory_t* memory;
	new_kernel_answer_t* answer = kmalloc(sizeof(*answer), GFP_ATOMIC);

	printk("received new kernel request cpu %d id %d\n", new_kernel_work->request->tgroup_home_cpu, new_kernel_work->request->tgroup_home_id);

	if (answer!=NULL) {
		memory = find_memory_entry(new_kernel_work->request->tgroup_home_cpu,
					   new_kernel_work->request->tgroup_home_id);
		if (memory != NULL) {
			printk("memory present cpu %d id %d\n", new_kernel_work->request->tgroup_home_cpu,new_kernel_work->request->tgroup_home_id);	
			down_write(&memory->kernel_set_sem);
			memory->kernel_set[new_kernel_work->request->header.from_cpu]= 1;
			memcpy(answer->my_set,memory->kernel_set,MAX_KERNEL_IDS*sizeof(int));
			up_write(&memory->kernel_set_sem);

			if (_cpu==new_kernel_work->request->tgroup_home_cpu){
				down_read(&memory->mm->mmap_sem);
				answer->vma_operation_index= memory->mm->vma_operation_index;
				//printk("%s answer->vma_operation_index %d\n",__func__,answer->vma_operation_index);
				up_read(&memory->mm->mmap_sem);
			}

		} else {
			PSPRINTK("memory not present\n");
			memset(answer->my_set,0,MAX_KERNEL_IDS*sizeof(int));
		}

		answer->tgroup_home_cpu= new_kernel_work->request->tgroup_home_cpu;
		answer->tgroup_home_id= new_kernel_work->request->tgroup_home_id;
		answer->header.type= PCN_KMSG_TYPE_PROC_SRV_NEW_KERNEL_ANSWER;
		answer->header.prio= PCN_KMSG_PRIO_NORMAL;
		//printk("just before send %s answer->vma_operation_index %d NR_CPU %d\n",__func__,answer->vma_operation_index,MAX_KERNEL_IDS);
		pcn_kmsg_send_long(new_kernel_work->request->header.from_cpu,
				   (struct pcn_kmsg_long_message*) answer,
				   sizeof(new_kernel_answer_t));
		//int ret=pcn_kmsg_send(new_kernel_work->request->header.from_cpu, (struct pcn_kmsg_long_message*) answer);
		//printk("%s send long ret is %d sizeof new_kernel_answer_t is %d size of header is %d\n",__func__,ret,sizeof(new_kernel_answer_t),sizeof(struct pcn_kmsg_hdr));
		kfree(answer);
	}

	pcn_kmsg_free_msg(new_kernel_work->request);
	kfree(work);
}

static int handle_new_kernel(struct pcn_kmsg_message* inc_msg)
{
	new_kernel_t* new_kernel= (new_kernel_t*)inc_msg;
	new_kernel_work_t* request_work;

	request_work = kmalloc(sizeof(new_kernel_work_t), GFP_ATOMIC);

	if (request_work) {
		request_work->request = new_kernel;
		INIT_WORK( (struct work_struct*)request_work, process_new_kernel);
		queue_work(new_kernel_wq, (struct work_struct*) request_work);
	}

	return 1;
}

///////////////////////////////////////////////////////////////////////////////
// thread pool functionalities
///////////////////////////////////////////////////////////////////////////////
static int create_kernel_thread_for_distributed_process(void *data);

static void update_thread_pull(struct work_struct* work)
{
	int i, count;
	count = count_data((data_header_t**)&thread_pull_head, &thread_pull_head_lock);

	for (i = 0; i < NR_THREAD_PULL - count; i++) {
		//	printk("%s creating thread pull %d\n", __func__, i);
		kernel_thread(create_kernel_thread_for_distributed_process, NULL, SIGCHLD);
	}

	kfree(work);
}

static void _create_thread_pull(struct work_struct* work)
{
	int i;
	create_thread_pull_t* msg;
	_remote_cpu_info_list_t *r, *temp;

	for (i = 0; i < NR_THREAD_PULL; i++){
		PSPRINTK("%s creating thread pull %d\n", __func__, i);
		kernel_thread(create_kernel_thread_for_distributed_process, NULL, SIGCHLD);
	}

	msg = kmalloc(sizeof(*msg), GFP_ATOMIC);
	if (!msg) {
		printk("%s Impossible to kmalloc", __func__);
		return;
	}

	msg->header.type = PCN_KMSG_TYPE_PROC_SRV_CREATE_THREAD_PULL;
	msg->header.prio = PCN_KMSG_PRIO_NORMAL;

	list_for_each_entry_safe(r, temp, &rlist_head, cpu_list_member) {
		i = r->_data._processor;
		pcn_kmsg_send_long(i, (struct pcn_kmsg_long_message*)msg,
			   sizeof(create_thread_pull_t) - sizeof(struct pcn_kmsg_hdr));
	}

	kfree(msg);
	kfree(work);
}

void create_thread_pull(void)
{
	static int only_one = 0;
	struct work_struct* work;

	if (only_one) return;

	work = kmalloc(sizeof(struct work_struct),GFP_ATOMIC);
	BUG_ON(!work);

	INIT_WORK(work, _create_thread_pull);
	queue_work(clone_wq, work);

	only_one++;
}

static int handle_thread_pull_creation(struct pcn_kmsg_message* inc_msg)
{
	printk("IN %s:%d\n", __func__, __LINE__);
	create_thread_pull();
	pcn_kmsg_free_msg(inc_msg);
	return 0;
}

/* return type:
 * 0 normal;
 * 1 flush pending operation
 * */
static int exit_distributed_process(memory_t* mm_data, int flush,thread_pull_t * my_thread_pull)
{
	struct task_struct *g;
	unsigned long flags;
	int is_last_thread_in_local_group = 1;
	int count = 0, i, status;

	lock_task_sighand(current, &flags);
	g = current;
	while_each_thread(current, g)
	{
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
		shadow_thread_t* my_shadow = NULL;
		struct work_struct* work;

		if (status == EXIT_THREAD) {
			printk("%s: ERROR: alive is 0 but status is exit thread (id %d, cpu %d)\n", __func__, current->tgroup_home_id, current->tgroup_home_cpu);
			return flush;
		}

		if (status == EXIT_PROCESS) {
			if (flush == 0) {
				//this is needed to flush the list of pending operation before die
				mm_data->arrived_op = 0;
#if 0 // beowulf
				vma_server_enqueue_vma_op(mm_data, 0, 1);
#else
				BUG();
#endif
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

		my_shadow = (shadow_thread_t*) pop_data( (data_header_t **)&(my_thread_pull->threads),
							 &(my_thread_pull->spinlock));

		while (my_shadow){
			my_shadow->thread->distributed_exit= EXIT_THREAD;
			wake_up_process(my_shadow->thread);
			kfree(my_shadow);
			my_shadow = (shadow_thread_t*) pop_data( (data_header_t **)&(my_thread_pull->threads),
								 &(my_thread_pull->spinlock));
		}
		remove_memory_entry(mm_data);
		mmput(mm_data->mm);
		kfree(mm_data);
#if STATISTICS
		PSPRINTK("%s: page_fault %i fetch %i local_fetch %i write %i read %i most_long_read %i invalid %i ack %i answer_request %i answer_request_void %i request_data %i most_written_page %i concurrent_writes %i most long write %i pages_allocated %i compressed_page_sent %i not_compressed_page %i not_compressed_diff_page %i  (id %d, cpu %d)\n", __func__ , 
			 page_fault_mio,fetch,local_fetch,write,read,most_long_read,invalid,ack,answer_request,answer_request_void, request_data,most_written_page, concurrent_write,most_long_write, pages_allocated,compressed_page_sent,not_compressed_page,not_compressed_diff_page, current->tgroup_home_id, current->tgroup_home_cpu);
#endif

		work = kmalloc(sizeof(struct work_struct), GFP_ATOMIC);
		if (work) {
			INIT_WORK( work, update_thread_pull);
			queue_work(clone_wq, work);
		}
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
#if 0 // beowulf
				vma_server_enqueue_vma_op(mm_data, 0, 1);
#else
				BUG();
#endif
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
 *		create_thread
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
struct task_struct* create_thread(int flags)
{
	struct task_struct *task = NULL;
	struct pt_regs regs;

	memset(&regs, 0, sizeof(regs));

	current->flags &= ~PF_KTHREAD;
	task = do_fork_for_main_kernel_thread(flags, 0, &regs, 0, NULL, NULL);
	current->flags |= PF_KTHREAD;

	if (task != NULL) {
		PSPRINTK("%s [-]: task = %p\n", __func__, task);
	} else {
		printk("%s [-]: do_fork failed, task = %p at %p\n", __func__, task, &task);
	}

	return task;
}

static void __spawn_shadow_threads(remote_thread_t *pool, int *spare_threads)
{
	int count;
    //printk("%s\n", __func__);

	count = count_data((data_header_t**)&(my_thread_pull->threads), &(my_thread_pull->spinlock));

	if (count != 0) return;

	while (count < *spare_threads) {
		shadow_thread_t* shadow = kmalloc(sizeof(shadow_thread_t), GFP_ATOMIC);
		BUG_ON(!shadow);

		count++;
		/* Ok, create the new process.. */
		shadow->thread = create_thread(CLONE_THREAD |
						   CLONE_SIGHAND	|
						   CLONE_VM	|
						   CLONE_UNTRACED);
		if (!IS_ERR(shadow->thread)) {
			printk("%s: WARN: push thread %d\n",
					__func__, shadow->thread->pid);

			push_data((data_header_t**)&(my_thread_pull->threads),
				  &(my_thread_pull->spinlock), (data_header_t*) shadow);
		} else {
			printk("%s: ERROR: not able to create shadow error %d\n",
					__func__, PTR_ERR_OR_ZERO(shadow->thread));
			kfree(shadow);
		}
	}
	*spare_threads = *spare_threads * 2;
}

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

static void main_for_distributed_kernel_thread(memory_t* mm_data, thread_pull_t * my_thread_pull)
{
	int count;
	// TODO: Need to explore how this has to be used
	//       added to port to Linux 3.12 API's
	//bool vma_locked = false;
	
	while (1) {
		int flush = 0;
		int spare_threads = 2;

		create_new_threads(my_thread_pull, &spare_threads);

		while (current->distributed_exit != EXIT_ALIVE) {
			flush = exit_distributed_process(mm_data, flush, my_thread_pull);
			msleep(1000);
		}
		while (mm_data->operation != VMA_OP_NOP && 
		       mm_data->mm->thread_op == current) {
			struct file* f;
			unsigned long populate = 0;
			unsigned long ret = 0;

			switch (mm_data->operation) {
			case VMA_OP_UNMAP:
				down_write(&mm_data->mm->mmap_sem);
				GET_UNMAP_IF_HOME(current, mm_data);
				ret = do_munmap(mm_data->mm, mm_data->addr, mm_data->len);
				PUT_UNMAP_IF_HOME(current, mm_data);
				up_write(&mm_data->mm->mmap_sem);
				break;

			case VMA_OP_MADVISE: { //this is only for MADV_REMOVE (thus write is 0)
				struct vm_area_struct *pvma;
				struct vm_area_struct *vma = find_vma(mm_data->mm, mm_data->addr);
				if (!vma || (vma->vm_start > mm_data->addr || vma->vm_end < mm_data->addr))
					printk("%s: ERROR VMA_OP_MADVISE cannot find VMA addr %lx start %lx end %lx\n",
							__func__, mm_data->addr, (vma ? vma->vm_start : 0), (vma ? vma->vm_end : 0));
				//write = madvise_need_mmap_write(behavior);
				//if (write)
				//	down_write(&(mm_data->mm->mmap_sem));
				//else
					down_read(&(mm_data->mm->mmap_sem));
				GET_UNMAP_IF_HOME(current, mm_data);
				ret = madvise_remove(vma, &pvma,
						mm_data->addr, (mm_data->addr + mm_data->len) );
				PUT_UNMAP_IF_HOME(current, mm_data);
				//if (write)
				//	up_write(&(mm_data->mm->mmap_sem));
				//else
					up_read(&(mm_data->mm->mmap_sem));
				break; }

			case VMA_OP_PROTECT:
				GET_UNMAP_IF_HOME(current, mm_data);
				ret = kernel_mprotect(mm_data->addr, mm_data->len, mm_data->prot);
				PUT_UNMAP_IF_HOME(current, mm_data);
				break;

			case VMA_OP_REMAP:
				// note that remap calls unmap --- thus is a nested operation ...
				down_write(&mm_data->mm->mmap_sem);
				GET_UNMAP_IF_HOME(current, mm_data);
				ret = kernel_mremap(mm_data->addr, mm_data->len, mm_data->new_len, 0, mm_data->new_addr);
				PUT_UNMAP_IF_HOME(current, mm_data);
				up_write(&mm_data->mm->mmap_sem);
				break;

			case VMA_OP_BRK:
				ret = -1;
				down_write(&mm_data->mm->mmap_sem);
				GET_UNMAP_IF_HOME(current, mm_data);
				ret = do_brk(mm_data->addr, mm_data->len);
				PUT_UNMAP_IF_HOME(current, mm_data);
				up_write(&mm_data->mm->mmap_sem);
				break;

			case VMA_OP_MAP:
				ret = -1;
				f = NULL;
				if (mm_data->path[0] != '\0') {
					f = filp_open(mm_data->path, O_RDONLY | O_LARGEFILE, 0);
					if (IS_ERR(f)) {
						printk("ERROR: cannot open file to map\n");
						break;
					}
				}
				down_write(&mm_data->mm->mmap_sem);
				GET_UNMAP_IF_HOME(current, mm_data);
				ret = do_mmap_pgoff(f, mm_data->addr, mm_data->len, mm_data->prot, 
						    mm_data->flags, mm_data->pgoff, &populate);
				PUT_UNMAP_IF_HOME(current, mm_data);
				up_write(&mm_data->mm->mmap_sem);
	
				if (mm_data->path[0] != '\0') {
					filp_close(f, NULL);
				}
				break;

			default:
				break;
			}
			mm_data->addr = ret;
			mm_data->operation = VMA_OP_NOP;

            PSPRINTK("%s: wake up %d (cpu %d id %d)\n",
            		__func__, mm_data->waiting_for_main->pid,
					current->tgroup_home_cpu, current->tgroup_home_id);
			wake_up_process(mm_data->waiting_for_main);
		}
		__set_task_state(current, TASK_UNINTERRUPTIBLE);

		count = count_data((data_header_t**)&(my_thread_pull->threads), &my_thread_pull->spinlock);
		if (count == 0 || current->distributed_exit != EXIT_ALIVE ||
		    (mm_data->operation != VMA_OP_NOP && mm_data->mm->thread_op == current)) {
			__set_task_state(current, TASK_RUNNING);
			continue;
		}
		schedule();
	}
}

static int create_kernel_thread_for_distributed_process_from_user_one(void *data)
{
	memory_t* entry = (memory_t*) data;
	thread_pull_t* my_thread_pull;
	int i, ret;

	printk("%s\n", __func__);
	current->main = 1;
	entry->main = current;

	if (!popcorn_ns) {
		if ((ret = build_popcorn_ns(0))) {
			printk(KERN_ERR"%s: WARN: build_popcorn returned error (%d)\n",
					__func__, ret);
		}
	}

	my_thread_pull = (thread_pull_t*) kmalloc(sizeof(thread_pull_t),
						  GFP_ATOMIC);
	if (!my_thread_pull) {
		printk(KERN_ERR"%s: ERROR: kmalloc thread pull\n", __func__);
		return -1;
	}

	raw_spin_lock_init(&(my_thread_pull->spinlock));
	my_thread_pull->main = current;
	my_thread_pull->memory = entry;
	my_thread_pull->threads= NULL;

	entry->thread_pull= my_thread_pull;

	//int count= count_data((data_header_t**)&(thread_pull_head), &thread_pull_head_lock);
	//printk("WARNING count is %d in %s\n",count,__func__);
        
	// Sharath: Increased the thread pool size
	for (i = 0; i < THREAD_POOL_SIZE; i++) {
		shadow_thread_t* shadow = (shadow_thread_t*) kmalloc(
			sizeof(shadow_thread_t), GFP_ATOMIC);
		if (shadow) {
			/* Ok, create the new process.. */
			shadow->thread = create_thread(CLONE_THREAD | CLONE_SIGHAND | CLONE_VM | CLONE_UNTRACED);
			if (!IS_ERR(shadow->thread)) {
				// printk("%s new shadow created\n",__func__);
				PSPRINTK("%s: push thread %d (%d)\n", __func__, shadow->thread->pid, i);
				push_data((data_header_t**)&(my_thread_pull->threads), &(my_thread_pull->spinlock),
					  (data_header_t*)shadow);
			} else {
				printk(KERN_ERR"%s: ERROR: not able to create shadow (%d)\n", __func__, i);
				kfree(shadow);
			}
		}
	}

	main_for_distributed_kernel_thread(entry,my_thread_pull);

	/* if here something went wrong...
	 */
	printk(KERN_ALERT"%s: ERROR: exited from main_for_distributed_kernel_thread\n", __func__);

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
		while (mm_data->main == NULL)
			schedule();

		lock_task_sighand(mm_data->main, &flags);
		mm_data->main->distributed_exit = EXIT_PROCESS;
		unlock_task_sighand(mm_data->main, &flags);

		wake_up_process(mm_data->main);
	}

	pcn_kmsg_free_msg(msg);
	kfree(work);
}

static void process_exiting_process_notification(struct work_struct* work)
{
	exit_work_t* request_work = (exit_work_t*) work;
	exiting_process_t* msg = request_work->request;

	unsigned int source_cpu = msg->header.from_cpu;
	struct task_struct *task;
	
	task = pid_task(find_get_pid(msg->prev_pid), PIDTYPE_PID);
	
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
	}
	else {
		printk(KERN_ALERT"%s: ERROR: task not found. Impossible to kill shadow (pid %d cpu %d)\n",
				__func__, msg->my_pid, source_cpu);
	}

	pcn_kmsg_free_msg(msg);
	kfree(work);
}

static int handle_thread_group_exited_notification(struct pcn_kmsg_message* inc_msg)
{
	exit_group_work_t* request_work;
	thread_group_exited_notification_t* request =
		(thread_group_exited_notification_t*) inc_msg;

	request_work = kmalloc(sizeof(exit_group_work_t), GFP_ATOMIC);

	if (request_work) {
		request_work->request = request;
		INIT_WORK( (struct work_struct*)request_work,
			   process_exit_group_notification);
		queue_work(exit_group_wq, (struct work_struct*) request_work);
	}
	return 1;
}

static int handle_exiting_process_notification(struct pcn_kmsg_message* inc_msg)
{
	exit_work_t* request_work;
	exiting_process_t* request = (exiting_process_t*) inc_msg;

/*
Antonio: this code is here after merging I don't have any clue about how the code is here
	if (response->vma_present == 1) {
		if (response->header.from_cpu != response->tgroup_home_cpu)
			printk("%s: WARN: a kernel that is not the server is sending the mapping (cpu %d id %d address 0x%lx)\n",
					__func__, response->header.from_cpu, response->tgroup_home_cpu, response->address);

		PSPRINTK("%s: response->vma_pesent %d reresponse->vaddr_start %lx response->vaddr_size %lx response->prot %lx response->vm_flags %lx response->pgoff %lx response->path %s response->fowner %d\n", 
			__func__, response->vma_present, response->vaddr_start , response->vaddr_size, (unsigned long)response->prot, response->vm_flags , response->pgoff, response->path,response->futex_owner);

		if (fetched_data->vma_present == 0) {
			PSPRINTK("Set vma\n");
			fetched_data->vma_present = 1;
			fetched_data->vaddr_start = response->vaddr_start;
			fetched_data->vaddr_size = response->vaddr_size;
			fetched_data->prot = response->prot;
			fetched_data->pgoff = response->pgoff;
			fetched_data->vm_flags = response->vm_flags;
			strcpy(fetched_data->path, response->path);
		} else {
			printk("%s: WARN: received more than one mapping %d f%dw%d (cpu %d id %d address 0x%lx) 0x%lx\n",
					__func__, fetched_data->vma_present, fetched_data->is_fetch, fetched_data->is_write,
					response->tgroup_home_cpu, response->tgroup_home_id, response->address, response);
		}
	}
*/
	request_work = kmalloc(sizeof(exit_work_t), GFP_ATOMIC);
	if (request_work) {
		request_work->request = request;
		INIT_WORK( (struct work_struct*)request_work,
			   process_exiting_process_notification);
		queue_work(exit_wq, (struct work_struct*) request_work);
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
static int handle_process_pairing_request(struct pcn_kmsg_message* inc_msg) {
	create_process_pairing_t* msg = (create_process_pairing_t*) inc_msg;
	unsigned int source_cpu = msg->header.from_cpu;
	struct task_struct* task;
	
	if (inc_msg == NULL) {
		return -1;
	}

	if (msg == NULL) {
		pcn_kmsg_free_msg(inc_msg);
		return -1;
	}

	task = find_task_by_vpid(msg->your_pid);
	if (task == NULL || task->represents_remote == 0) {
		return -1;
	}
	task->next_cpu = source_cpu;
	task->next_pid = msg->my_pid;
	task->executing_for_remote = 0;
	pcn_kmsg_free_msg(inc_msg);
	return 1;
}

static int handle_remote_thread_count_response(struct pcn_kmsg_message* inc_msg)
{
	remote_thread_count_response_t* msg =
			(remote_thread_count_response_t *)inc_msg;
	count_answers_t* data = 
			find_count_entry(msg->tgroup_home_cpu, msg->tgroup_home_id);
	unsigned long flags;
	struct task_struct* to_wake = NULL;

	PSPRINTK("%s: entered - cpu{%d}, id{%d}, count{%d}\n", __func__, msg->tgroup_home_cpu, msg->tgroup_home_id, msg->count);

	if (data == NULL) {
		printk("%s: ERROR: unable to find remote thread count data"
				"(cpu %d id %d)\n",
				__func__, msg->tgroup_home_cpu, msg->tgroup_home_id);
		pcn_kmsg_free_msg(inc_msg);
		return -1;
	}

	raw_spin_lock_irqsave(&(data->lock), flags);

	// Register this response.
	data->responses++;
	data->count += msg->count;

	if (data->responses >= data->expected_responses)
		to_wake = data->waiting;

	raw_spin_unlock_irqrestore(&(data->lock), flags);

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

	PSPRINTK("%s: entered - cpu{%d}, id{%d}\n",
			__func__, request->tgroup_home_cpu, request->tgroup_home_id);

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

	memory = find_memory_entry(request->tgroup_home_cpu,
					     request->tgroup_home_id);
	if (memory != NULL) {
		struct task_struct *t;
		while (memory->main == NULL)
			schedule();

		t = memory->main;
		while_each_thread(memory->main, t) {
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
#if 0 // beowulf
static void process_back_migration(struct work_struct* work)
{
	back_mig_work_t* info_work = (back_mig_work_t*) work;
	back_migration_request_t* request = info_work->back_mig_request;

	struct task_struct * task = pid_task(find_get_pid(request->prev_pid), PIDTYPE_PID);
	
	PSPRINTK("%s: PID %d, prev_pid: %d\n",__func__, task->pid, request->prev_pid);
	if (task!=NULL
			&& (task->next_pid == request->placeholder_pid)
			&& (task->next_cpu == request->header.from_cpu)
			&& (task->represents_remote == 1)) {
		// TODO: Handle return values
		restore_thread_info(task, &request->arch);
		initialize_thread_retval(task, 0);

		task->prev_cpu = request->header.from_cpu;
		task->prev_pid = request->placeholder_pid;
		task->personality = request->personality;

		//	sigorsets(&task->blocked,&task->blocked,&request->remote_blocked) ;
		//	sigorsets(&task->real_blocked,&task->real_blocked,&request->remote_real_blocked);
		//	sigorsets(&task->saved_sigmask,&task->saved_sigmask,&request->remote_saved_sigmask);
		//	task->pending = request->remote_pending;
		//	task->sas_ss_sp = request->sas_ss_sp;
		//	task->sas_ss_size = request->sas_ss_size;

		// int cnt = 0;
		//	for (cnt = 0; cnt < _NSIG; cnt++)
		//		task->sighand->action[cnt] = request->action[cnt];
#if MIGRATE_FPU
		// TODO: Handle return values
		restore_fpu_info(task, &request->arch);
#endif

		task->executing_for_remote = 1;
		task->represents_remote = 0;
		wake_up_process(task);
	}
	else {
		printk("%s: ERROR: task not found. Impossible to re-run shadow (pid %d cpu %d)\n",
				__func__, request->placeholder_pid, request->header.from_cpu);
	}
	pcn_kmsg_free_msg(request);
	kfree(work);
}
#endif

static int handle_back_migration(struct pcn_kmsg_message* inc_msg)
{
	back_migration_request_t* request= (back_migration_request_t*) inc_msg;
	memory_t* memory = NULL;
	struct task_struct *task;

#if MIGRATION_PROFILE
	migration_start = ktime_get();
#endif
	//for synchronizing migratin threads

	PSPRINTK(" IN %s:%d values - %d %d\n", __func__, __LINE__,request->tgroup_home_cpu, request->tgroup_home_id);
	memory = find_memory_entry(request->tgroup_home_cpu,
				   request->tgroup_home_id);
	if (memory) {
		atomic_inc(&(memory->pending_back_migration));
		/*
		int app= atomic_add_return(1,&(memory->pending_back_migration));
		if (app==57)
			atomic_set(&(memory->pending_back_migration),114);
		*/
	} else {
		printk("%s: ERROR: back migration did not find a memory_t struct! (cpu %d id %d)\n",
				__func__, request->tgroup_home_cpu, request->tgroup_home_id);
	}
	/*
	back_mig_work_t* work;
	work = (back_mig_work_t*) kmalloc(sizeof(back_mig_work_t), GFP_ATOMIC);
	if (work) {
		INIT_WORK( (struct work_struct*)work, process_back_migration);
		work->back_mig_request = request;
		queue_work(clone_wq, (struct work_struct*) work);
	}
	*/

	//temporary code to check if the back migration can be faster
	task = pid_task(find_get_pid(request->prev_pid), PIDTYPE_PID);

	if (task != NULL && (task->next_pid == request->placeholder_pid)
	    && (task->next_cpu == request->header.from_cpu)
	    && (task->represents_remote == 1)) {
		// TODO: Handle return values
		restore_thread_info(task, &request->arch);
		initialize_thread_retval(task, 0);

		task->prev_cpu = request->header.from_cpu;
		task->prev_pid = request->placeholder_pid;
		task->personality = request->personality;
		task->executing_for_remote = 1;
		task->represents_remote = 0;
		wake_up_process(task);

#if MIGRATION_PROFILE
		migration_end = ktime_get();
#if defined(CONFIG_ARM64)
  		printk(KERN_ERR "Time for x86->arm back migration - ARM side: %ld ns\n", (unsigned long)ktime_to_ns(ktime_sub(migration_end, migration_start)));
#else
		printk(KERN_ERR "Time for arm->x86 back migration - x86 side: %ld ns\n", (unsigned long)ktime_to_ns(ktime_sub(migration_end, migration_start)));
#endif
#endif
	} else {
		printk("%s: ERROR: task not found. Impossible to re-run shadow (pid %d cpu %d)\n",
				__func__, request->placeholder_pid, request->header.from_cpu);
	}

	pcn_kmsg_free_msg(request);
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
			while (entry->main == NULL)
				schedule();
		} else {
			printk("%s: ERROR: Mapping disappeared, cannot wake up main thread... (cpu %d id %d)\n",
					__func__, tsk->tgroup_home_cpu, tsk->tgroup_home_id);
			return -1;
		}

		lock_task_sighand(tsk, &flags);
		tsk->distributed_exit = EXIT_THREAD;
		if (entry->main->distributed_exit == EXIT_ALIVE)
			entry->main->distributed_exit = EXIT_THREAD;
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
		wake_up_process(entry->main);
	}
	return tx_ret;
}

/**
 * Create a pairing between a newly created delegate process and the
 * remote placeholder process.  This function creates the local
 * pairing first, then sends a message to the originating cpu
 * so that it can do the same.
 */
static int process_server_notify_delegated_subprocess_starting(pid_t pid,
							pid_t remote_pid, int remote_cpu)
{
	create_process_pairing_t* msg;
	int tx_ret = -1;

	msg= (create_process_pairing_t*) kmalloc(sizeof(create_process_pairing_t),GFP_ATOMIC);
	if (!msg)
		return -1;
	// Notify remote cpu of pairing between current task and remote
	// representative task.
	msg->header.type = PCN_KMSG_TYPE_PROC_SRV_CREATE_PROCESS_PAIRING;
	msg->header.prio = PCN_KMSG_PRIO_NORMAL;
	msg->your_pid = remote_pid;
	msg->my_pid = pid;

	tx_ret = pcn_kmsg_send_long(remote_cpu, (struct pcn_kmsg_long_message *) msg,
				    sizeof(create_process_pairing_t));
	kfree(msg);

	return tx_ret;

}

///////////////////////////////////////////////////////////////////////////////
// task functions
///////////////////////////////////////////////////////////////////////////////

int process_server_dup_task(struct task_struct* orig, struct task_struct* task)
{
	unsigned long flags;

	task->executing_for_remote = 0;
	task->represents_remote = 0;
	task->distributed_exit = EXIT_ALIVE;
	task->tgroup_distributed = 0;
	task->prev_cpu = -1;
	task->next_cpu = -1;
	task->prev_pid = -1;
	task->next_pid = -1;
	task->tgroup_home_cpu = -1;
	task->tgroup_home_id = -1;
	task->main = 0;
	task->group_exit = -1;
	task->surrogate = -1; // this is for futex
	task->group_exit= -1;
	task->uaddr = 0;
	task->origin_pid = -1;
	// If the new task is not in the same thread group as the parent,
	// then we do not need to propagate the old thread info.
	if (orig->tgid != task->tgid) {
		return 1;
	}

	lock_task_sighand(orig, &flags);
	// This is important.  We want to make sure to keep an accurate record
	// of which cpu and thread group the new thread is a part of.
	if (orig->tgroup_distributed == 1) {
		task->tgroup_home_cpu = orig->tgroup_home_cpu;
		task->tgroup_home_id = orig->tgroup_home_id;
		task->tgroup_distributed = 1;
		PSPRINTK("%s: INFO: dup task (cpu %d id %d)\n", __func__, task->tgroup_home_cpu, task->tgroup_home_id);
	}
	unlock_task_sighand(orig, &flags);

	return 1;
}
/*
 * Send a message to <dst_cpu> for migrating back a task <task>.
 * This is a back migration => <task> must already been migrated at least once in <dst_cpu>.
 * It returns -1 in error case.
 */
static int do_back_migration(struct task_struct *task, int dst_cpu,
			     struct pt_regs *regs, void __user *uregs)
{
	unsigned long flags;
	int ret;
	back_migration_request_t* request;
	int cnt = 0;

	request= (back_migration_request_t*) kmalloc(sizeof(back_migration_request_t), GFP_ATOMIC);
	if (request==NULL)
		return -1;

	PSPRINTK("%s\n", __func__);

	//printk("%s entered dst{%d}\n",__func__,dst_cpu);
	request->header.type = PCN_KMSG_TYPE_PROC_SRV_BACK_MIG_REQUEST;
	request->header.prio = PCN_KMSG_PRIO_NORMAL;
	request->tgroup_home_cpu = task->tgroup_home_cpu;
	request->tgroup_home_id = task->tgroup_home_id;
	request->placeholder_pid = task->pid;
	request->placeholder_tgid = task->tgid;
	request->back=1;
	request->prev_pid= task->prev_pid;
	request->personality = task->personality;

	/*mklinux_akshay*/
	request->origin_pid = task->origin_pid;
	request->remote_blocked = task->blocked;
	request->remote_real_blocked = task->real_blocked;
	request->remote_saved_sigmask = task->saved_sigmask;
	request->remote_pending = task->pending;
	request->sas_ss_sp = task->sas_ss_sp;
	request->sas_ss_size = task->sas_ss_size;
	for (cnt = 0; cnt < _NSIG; cnt++)
		request->action[cnt] = task->sighand->action[cnt];

	// TODO: Handle return value
	save_thread_info(task, regs, &request->arch, uregs);

#if MIGRATE_FPU
	// TODO: Handle return value
	save_fpu_info(task, &request->arch);
#endif

	lock_task_sighand(task, &flags);

	if (task->tgroup_distributed == 0) {
		unlock_task_sighand(task, &flags);
		printk("%s: ERROR: back migrating thread of not tgroup_distributed process (cpu %d id %d)\n",
				__func__, task->tgroup_home_cpu, task->tgroup_home_id);
		kfree(request);
		return -1;
	}

	task->represents_remote = 1;
	task->next_cpu = task->prev_cpu;
	task->next_pid = task->prev_pid;
	task->executing_for_remote= 0;
	unlock_task_sighand(task, &flags);

#if MIGRATION_PROFILE
	migration_end = ktime_get();
        printk(KERN_ERR "Time for x86->arm back migration - x86 side: %ld ns\n", (unsigned long)ktime_to_ns(ktime_sub(migration_end,migration_start)));
#endif

	ret = pcn_kmsg_send_long(dst_cpu,
				 (struct pcn_kmsg_long_message *)request,
				 sizeof(clone_request_t));

	kfree(request);
	return ret;
}

/*
 * Send a message to <dst_cpu> for migrating a task <task>.
 * This function will ask the remote cpu to create a thread to host task.
 * It returns -1 in error case.
 */
//static
int do_migration(struct task_struct* task, int dst_cpu,
		 struct pt_regs * regs, int* is_first,
                 void __user *uregs)
{
	pid_t kthread_main = 0;
	clone_request_t* request;
	int tx_ret = -1;
	struct task_struct* tgroup_iterator = NULL;
	char path[256] = { 0 };
	char* rpath;
	memory_t* entry;
	int first = 0;
	unsigned long flags;
	int cnt = 0;

	request = kmalloc(sizeof(clone_request_t), GFP_ATOMIC);
	if (request == NULL) {
		return -1;
	}
	*is_first=0;
	PSPRINTK("%s: PID %d\n", __func__, task->pid);
	// Build request
	request->header.type = PCN_KMSG_TYPE_PROC_SRV_CLONE_REQUEST;
	request->header.prio = PCN_KMSG_PRIO_NORMAL;

	// struct mm_struct --------------------------------------------------------
	rpath = d_path(&task->mm->exe_file->f_path, path, 256);
	strncpy(request->exe_path, rpath, 512);
	request->stack_start = task->mm->start_stack;
	request->start_brk = task->mm->start_brk;
	request->brk = task->mm->brk;
	request->env_start = task->mm->env_start;
	request->env_end = task->mm->env_end;
	request->arg_start = task->mm->arg_start;
	request->arg_end = task->mm->arg_end;
	request->start_code = task->mm->start_code;
	request->end_code = task->mm->end_code;
	request->start_data = task->mm->start_data;
	request->end_data = task->mm->end_data;
	request->def_flags = task->mm->def_flags;
	request->popcorn_vdso = task->mm->context.popcorn_vdso;
	// struct task_struct ------------------------------------------------------
	request->placeholder_pid = task->pid;
	request->placeholder_tgid = task->tgid;
	/*mklinux_akshay*/
	if (task->prev_pid == -1)
		request->origin_pid = task->pid;
	else
		request->origin_pid = task->origin_pid;
	request->remote_blocked = task->blocked;
	request->remote_real_blocked = task->real_blocked;
	request->remote_saved_sigmask = task->saved_sigmask;
	request->remote_pending = task->pending;
	request->sas_ss_sp = task->sas_ss_sp;
	request->sas_ss_size = task->sas_ss_size;
	for (cnt = 0; cnt < _NSIG; cnt++)
		request->action[cnt] = task->sighand->action[cnt];

	request->back=0;

	/*mklinux_akshay*/
	if (task->prev_pid==-1)
		task->origin_pid=task->pid;
	else
		task->origin_pid=task->origin_pid;

	request->personality = task->personality;

	// TODO: Handle return value
	if (save_thread_info(task, regs, &request->arch, uregs) != 0) {
		kfree(request);
		return -EINVAL;
	}

#if MIGRATE_FPU
	save_fpu_info(task, &request->arch);
#endif

	/*I use siglock to coordinate the thread group.
	 *This process is becoming a distributed one if it was not already.
	 *The first migrating thread has to create the memory entry to handle page requests,
	 *and fork the main kernel thread of this process.
	 */
	lock_task_sighand(task, &flags);

	if (task->tgroup_distributed == 0) { // convert the task to distributed
		task->tgroup_distributed = 1;
		task->tgroup_home_id = task->tgid;
		task->tgroup_home_cpu = _cpu;
		printk("%s: INFO: migrate task (cpu %d id %d)\n", __func__, task->tgroup_home_cpu, task->tgroup_home_id);

		entry = (memory_t*) kmalloc(sizeof(memory_t), GFP_ATOMIC);
		if (!entry){
			unlock_task_sighand(task, &flags);
			printk("%s: ERROR: Impossible allocate memory_t while migrating thread (cpu %d id %d)\n",
					__func__, task->tgroup_home_cpu, task->tgroup_home_id);
			return -1;
		}

		INIT_LIST_HEAD(&entry->list);
		entry->mm = task->mm;
		atomic_inc(&entry->mm->mm_users);
		entry->tgroup_home_cpu = task->tgroup_home_cpu;
		entry->tgroup_home_id = task->tgroup_home_id;
		entry->alive = 1;
		entry->main = NULL;
		atomic_set(&(entry->pending_migration),0);
		atomic_set(&(entry->pending_back_migration),0);
		entry->operation = VMA_OP_NOP;
		entry->waiting_for_main = NULL;
		entry->waiting_for_op = NULL;
		entry->arrived_op = 0;
		entry->my_lock = 0;
		memset(entry->kernel_set,0,MAX_KERNEL_IDS*sizeof(int));
		entry->kernel_set[_cpu]= 1;
		init_rwsem(&entry->kernel_set_sem);
		entry->setting_up= 0;
		init_rwsem(&task->mm->distribute_sem);
		task->mm->distr_vma_op_counter = 0;
		task->mm->was_not_pushed = 0;
		task->mm->thread_op = NULL;
		task->mm->vma_operation_index = 0;
		task->mm->distribute_unmap = 1;
		add_memory_entry(entry);

		PSPRINTK("memory entry added cpu %d id %d\n", entry->tgroup_home_cpu, entry->tgroup_home_id);
		first=1;
		*is_first = 1;

		tgroup_iterator = task;
		while_each_thread(task, tgroup_iterator)
		{
			tgroup_iterator->tgroup_home_id = task->tgroup_home_id;
			tgroup_iterator->tgroup_home_cpu = task->tgroup_home_cpu;
			tgroup_iterator->tgroup_distributed = 1;
		};
	}

	task->represents_remote = 1;

	unlock_task_sighand(task, &flags);

	request->tgroup_home_cpu = task->tgroup_home_cpu;
	request->tgroup_home_id = task->tgroup_home_id;

#if MIGRATION_PROFILE
	migration_end = ktime_get();
        printk(KERN_ERR "Time for x86->arm migration - x86 side: %ld ns\n", (unsigned long)ktime_to_ns(ktime_sub(migration_end,migration_start)));
#endif
	tx_ret = pcn_kmsg_send_long(dst_cpu,
				    (struct pcn_kmsg_long_message *)request,
				    sizeof(clone_request_t));
#else
	tx_ret = handle_clone_request((void *)request);
#endif

	if (create_kernel_proxy) {
		pid_t pid = kernel_thread_popcorn(
				create_kernel_thread_for_distributed_process_from_user_one,
				entry, CLONE_THREAD | CLONE_SIGHAND | CLONE_VM | SIGCHLD);

	if (first) {
		PSPRINTK(KERN_EMERG"%s: Creating kernel thread\n", __func__);
		// Sharath: In Linux 3.12 this API does not allow a kernel thread to be 
		//          created on a user context
		/*return_value = kernel_thread(create_kernel_thread_for_distributed_process_from_user_one,
		  entry, CLONE_THREAD | CLONE_SIGHAND | CLONE_VM | SIGCHLD);*/
		kthread_main = kernel_thread_popcorn(create_kernel_thread_for_distributed_process_from_user_one,
						     entry, CLONE_THREAD | CLONE_SIGHAND | CLONE_VM | SIGCHLD);
	}

	kfree(request);
	return tx_ret;
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
	int first = 0;
	int ret = 0;

	PSPRINTK("%s: pid %d tgid %d"
			"task->tgroup_home_id %d task->tgroup_home_cpu %d\n",
			__func__, current->pid, current->tgid,
			task->tgroup_home_id, task->tgroup_home_cpu);

	if (task->prev_cpu == dst_cpu) {
		ret = do_back_migration(task, dst_cpu, regs, uregs);
	} else {
		ret = do_migration(task, dst_cpu, regs, &first, uregs);
	}

	if (ret == -1) {
		return PROCESS_SERVER_CLONE_FAIL;
	}

	//printk(KERN_ALERT"%s clone request sent ret{%d}\n", __func__,ret);
	__set_task_state(task, TASK_UNINTERRUPTIBLE);
	return PROCESS_SERVER_CLONE_SUCCESS;
}

void sleep_shadow()
{
	memory_t* memory = NULL;
    PSPRINTK("%s pid %d\n", __func__, current->pid);

	if (current->executing_for_remote == 0 && current->distributed_exit== EXIT_NOT_ACTIVE) {
		do {
			set_task_state(current, TASK_INTERRUPTIBLE);
			schedule_timeout(HZ*20); // we take
			if (current->state != TASK_RUNNING) {
				printk("%s: ERROR, linux documentation sucks (current state is 0x%lx)\n", __func__, current->state);
				set_task_state(current, TASK_RUNNING);
			}
		} while (current->executing_for_remote == 0 && current->distributed_exit== EXIT_NOT_ACTIVE);
	}

	PSPRINTK("%s: WARN woken up pid %d\n", __func__, current->pid);
	if (current->distributed_exit!= EXIT_NOT_ACTIVE){
		current->represents_remote = 0;
		do_exit(0);
	}

	current->distributed_exit= EXIT_ALIVE;
	current->represents_remote = 0;

	// Notify of PID/PID pairing.
	process_server_notify_delegated_subprocess_starting(current->pid, current->prev_pid, current->prev_cpu);

	//this force the task to wait that the main correctly set up the memory
	while (current->tgroup_distributed != 1) {
		msleep(1);
	}

	PSPRINTK("%s main set up pid %d\n", __func__, current->pid);
	memory = find_memory_entry(current->tgroup_home_cpu,
				   current->tgroup_home_id);
	if (!memory) {
		printk("%s: WARN: cannot find memory_t (cpu %d id %d)\n",
				__func__, current->tgroup_home_cpu, current->tgroup_home_id);
	}
	else
		memory->alive = 1;

	// TODO: Handle return values
	update_thread_info(current);

	//to synchronize migrations...
	// Sharath: Thread won't wait till all 57 threads are migrated
	/*while (atomic_read(&(memory->pending_migration))<57) {
	  msleep(1);
	  }*/
	/*int app= atomic_add_return(-1,&(memory->pending_migration));
	  if (app==57)
	  atomic_set(&(memory->p/n),0);*/
	atomic_dec(&(memory->pending_migration));

	//to synchronize migrations...
	while (atomic_read(&(memory->pending_migration))!=0) {
		msleep(1);
	}

#if MIGRATE_FPU
	// TODO: Handle return values
	update_fpu_info(current);
#endif
	PSPRINTK("%s return pid %d\n", __func__, current->pid);
}

///////////////////////////////////////////////////////////////////////////////
// thread specific handling
///////////////////////////////////////////////////////////////////////////////
static int create_user_thread_for_distributed_process(clone_request_t* clone_data,
					       thread_pull_t* my_thread_pull)
{
	shadow_thread_t* my_shadow;
	struct task_struct* task;
	int ret;

	my_shadow = (shadow_thread_t*)pop_data((data_header_t**)&(my_thread_pull->threads),
						&(my_thread_pull->spinlock));
	if (!my_shadow) {
		printk("%s: ERROR: no shadows found! (cpu %d id %ld)\n",
				__func__, clone_data->header.from_cpu,
				(unsigned long)clone_data->placeholder_pid);
		wake_up_process(my_thread_pull->main);
		return -1;
	}

	task = my_shadow->thread;
	PSPRINTK("%s: pop_data pid %d\n", __func__, task->pid);
	if (task == NULL) {
		printk("%s, ERROR task is NULL\n", __func__);
		return -1;
	}
	if (!popcorn_ns) {
		printk("%s: ERROR: no popcorn_ns when forking migrating threads\n", __func__);
		return -1;
	}
	/* if we are already attached, let's skip the unlinking and linking */
	if (task->nsproxy->cpu_ns != popcorn_ns) {
		//i TODO temp fix or of all active cpus?! ---- TODO this must be fixed is not acceptable
		do_set_cpus_allowed(task, cpu_online_mask);
		put_cpu_ns(task->nsproxy->cpu_ns);
		task->nsproxy->cpu_ns = get_cpu_ns(popcorn_ns);
	}
	//associate the task with the namespace
	ret = associate_to_popcorn_ns(task);
	if (ret) {
		printk(KERN_ERR "%s: ERROR: associate_to_popcorn_ns returned: %d\n",
				__func__,ret);
	}

	__copy_task_comm(task, clone_data->exe_path);

	// set thread info
	// TODO: Handle return values
	restore_thread_info(task, &clone_data->arch);
	initialize_thread_retval(task, 0);

	task->prev_cpu = clone_data->header.from_cpu;
	task->prev_pid = clone_data->placeholder_pid;
	task->personality = clone_data->personality;
	//	task->origin_pid = clone_data->origin_pid;
	//	sigorsets(&task->blocked,&task->blocked,&clone_data->remote_blocked) ;
	//	sigorsets(&task->real_blocked,&task->real_blocked,&clone_data->remote_real_blocked);
	//	sigorsets(&task->saved_sigmask,&task->saved_sigmask,&clone_data->remote_saved_sigmask);
	//	task->pending = clone_data->remote_pending;
	//	task->sas_ss_sp = clone_data->sas_ss_sp;
	//	task->sas_ss_size = clone_data->sas_ss_size;
	//	int cnt = 0;
	//	for (cnt = 0; cnt < _NSIG; cnt++)
	//		task->sighand->action[cnt] = clone_data->action[cnt];
#if MIGRATE_FPU
	restore_fpu_info(task, &clone_data->arch);
#endif
	//the task will be activated only when task->executing_for_remote==1
	task->executing_for_remote = 1;
	PSPRINTK("%s: wake up %d\n", __func__, task->pid);
	wake_up_process(task);

#if MIGRATION_PROFILE
	migration_end = ktime_get();
	printk(KERN_ERR "Time for arm->x86 migration - x86 side: %ld ns\n", (unsigned long)ktime_to_ns(ktime_sub(migration_end,migration_start)));
#endif
	//printk("%s pc %lx\n", __func__, (&clone_data->arch)->migration_pc);
	//printk("%s pc %lx\n", __func__, task_pt_regs(task)->pc);
	//printk("%s sp %lx\n", __func__, task_pt_regs(task)->sp);
	//printk("%s bp %lx\n", __func__, task_pt_regs(task)->bp);

	printk("####### MIGRATED - PID: %ld to %ld CPU: %d to %d \n",
			(unsigned long)task->prev_pid, (unsigned long)task->pid, task->prev_cpu, _cpu);

	kfree(my_shadow);
	pcn_kmsg_free_msg(clone_data);
	return 0;
}

static int create_kernel_thread_for_distributed_process(void *data)
{
	thread_pull_t* my_thread_pull;
	struct cred * new;
	struct mm_struct *mm;
	memory_t* entry;
	struct task_struct* tgroup_iterator = NULL;
	unsigned long flags;
	int i;
	new_kernel_t* new_kernel_msg;
	_remote_cpu_info_list_t *objPtr, *tmpPtr;

	PSPRINTK("%s: current %d\n", __func__, current->pid);
    /* dump_stack(); */

	spin_lock_irq(&current->sighand->siglock);
	flush_signal_handlers(current, 1);
	spin_unlock_irq(&current->sighand->siglock);

	set_cpus_allowed_ptr(current, cpu_all_mask);
	set_user_nice(current, 0);
	new = prepare_kernel_cred(current);
	commit_creds(new);

	mm = mm_alloc();
	if (!mm) {
		printk("%s: ERROR: Impossible allocate new mm_struct\n", __func__);
		return -1;
	}

	init_new_context(current, mm);
	arch_pick_mmap_layout(mm);

	exec_mmap(mm);
	set_fs(USER_DS);
	current->flags &= ~(PF_RANDOMIZE | PF_KTHREAD);
	flush_thread();
	flush_signal_handlers(current, 0);

	current->main = 1;

	if (!popcorn_ns) {
		if ((build_popcorn_ns(0)))
			printk("%s: ERROR: build_popcorn returned error\n", __func__);
	}

	my_thread_pull = kmalloc(sizeof(thread_pull_t), GFP_ATOMIC);
	BUG_ON(!my_thread_pull);

	init_rwsem(&current->mm->distribute_sem);
	current->mm->distr_vma_op_counter = 0;
	current->mm->was_not_pushed = 0;
	current->mm->thread_op = NULL;
	current->mm->vma_operation_index = 0;
	current->mm->distribute_unmap = 1;
	my_thread_pull->memory = NULL;
	my_thread_pull->threads = NULL;
	raw_spin_lock_init(&(my_thread_pull->spinlock));
	my_thread_pull->main = current;

	push_data((data_header_t**)&(thread_pull_head), &thread_pull_head_lock, (data_header_t *)my_thread_pull);

	// Sharath: Increased the thread pool size
	for (i = 0; i < THREAD_POOL_SIZE; i++) {
		shadow_thread_t* shadow = kmalloc(sizeof(shadow_thread_t), GFP_ATOMIC);
		BUG_ON(!shadow);

		/* Ok, create the new process.. */
		shadow->thread = create_thread(CLONE_THREAD | CLONE_SIGHAND | CLONE_VM | CLONE_UNTRACED);
		if (!IS_ERR(shadow->thread)) {
			/* printk("%s: push thread %d\n", __func__, shadow->thread->pid); */
			push_data((data_header_t**)&(my_thread_pull->threads), &(my_thread_pull->spinlock), (data_header_t *)shadow);
		} else {
			printk("%s: ERROR: not able to create shadow (cpu %d id %d)\n",
					__func__, current->tgroup_home_cpu, current->tgroup_home_id);
			kfree(shadow);
		}
	}

	if (my_thread_pull->memory == NULL) {
		do {
			set_task_state(current, TASK_INTERRUPTIBLE);
			schedule_timeout(HZ*20); // we take
			if (current->state != TASK_RUNNING) {
				printk("%s: ERROR, linux documentation sucks (current state is 0x%lx)\n", __func__, current->state);
				set_task_state(current, TASK_RUNNING);
			}
		} while (my_thread_pull->memory == NULL);
	}
    PSPRINTK("%s: after memory current pid %d\n", __func__, current->pid);

	printk("new thread pull aquired! %p\n", my_thread_pull->memory);
	entry = my_thread_pull->memory;
	entry->operation = VMA_OP_NOP;
	entry->waiting_for_main = NULL;
	entry->waiting_for_op = NULL;
	entry->arrived_op = 0;
	entry->my_lock = 0;
	atomic_set(&(entry->pending_back_migration), 0);
	memset(entry->kernel_set, 0, MAX_KERNEL_IDS * sizeof(int));
	entry->kernel_set[_cpu] = 1;
	init_rwsem(&entry->kernel_set_sem);

	new_kernel_msg = kmalloc(sizeof(new_kernel_t), GFP_ATOMIC);
	BUG_ON(!new_kernel_msg);

	new_kernel_msg->tgroup_home_cpu = current->tgroup_home_cpu;
	new_kernel_msg->tgroup_home_id = current->tgroup_home_id;

	new_kernel_msg->header.type = PCN_KMSG_TYPE_PROC_SRV_NEW_KERNEL;
	new_kernel_msg->header.prio = PCN_KMSG_PRIO_NORMAL;

	entry->exp_answ = 0;
	entry->answers = 0;
	spin_lock_init(&(entry->lock_for_answer));
	
	/*
	 * inform all kernel that a new distributed process is present here
	 * the list does not include the current processor group descirptor (TODO)
	 */
	list_for_each_entry_safe(objPtr, tmpPtr, &rlist_head, cpu_list_member) {
		i = objPtr->_data._processor;
		PSPRINTK("sending new kernel message to %d\n",i);
		PSPRINTK("cpu %d id %d\n",new_kernel_msg->tgroup_home_cpu,
				new_kernel_msg->tgroup_home_id);
		printk("memory pointer from list is %p\n", find_memory_entry(
					new_kernel_msg->tgroup_home_cpu,
					new_kernel_msg->tgroup_home_id));

		if (pcn_kmsg_send_long(i,
			   (struct pcn_kmsg_long_message*)(new_kernel_msg),
			   sizeof(new_kernel_t)) != -1) {
			// Message delivered
			entry->exp_answ++;
		}
	}
	PSPRINTK("%s: sent %d new kernel messages, current %d\n", __func__, entry->exp_answ, current->pid);

	while (entry->exp_answ != entry->answers) {
		set_task_state(current, TASK_UNINTERRUPTIBLE);
		if (entry->exp_answ != entry->answers) {
			schedule();
		}
		set_task_state(current, TASK_RUNNING);
	}

	PSPRINTK("%s: received all answers\n", __func__);
	kfree(new_kernel_msg);

	lock_task_sighand(current, &flags);

	tgroup_iterator = current;
	while_each_thread(current, tgroup_iterator)
	{
		tgroup_iterator->tgroup_home_id = current->tgroup_home_id;
		tgroup_iterator->tgroup_home_cpu = current->tgroup_home_cpu;
		tgroup_iterator->tgroup_distributed = 1;
	};

	unlock_task_sighand(current, &flags);

	//printk("woke up everybody\n");
	entry->alive = 1;
	entry->setting_up = 0;

	//struct file* f;
	//f = filp_open("/bin/test_thread_migration", O_RDONLY | O_LARGEFILE, 0);
	//if (IS_ERR(f))
	//      printk("Impossible to open file /bin/test_thread_migration error is %d\n",PTR_ERR(f));
	//else
	//      filp_close(f,NULL);

	main_for_distributed_kernel_thread(entry,my_thread_pull);

	printk("%s: ERROR: exited from main_for_distributed_kernel_thread\n", __func__);
	return 0;
}

static void clone_remote_thread_failed(struct work_struct *work)
{
	struct file *f;
	memory_t *entry;
	unsigned long flags;
	clone_work_t *clone_work = (clone_work_t *)work;
	clone_request_t *clone = clone_work->request;
	thread_pull_t* my_thread_pull = NULL;

	entry = find_memory_entry(clone->tgroup_home_cpu, clone->tgroup_home_id);
	do {
		my_thread_pull = (thread_pull_t*)
			pop_data((data_header_t**)&thread_pull_head, &thread_pull_head_lock);
		msleep(10);
	} while (my_thread_pull == NULL);

	printk("%s found a thread pull %lx\n", __func__, clone->stack_start);

	entry->thread_pull = my_thread_pull;
	entry->main = my_thread_pull->main;
	entry->mm = my_thread_pull->main->mm;
	//printk("%s main kernel thread is %p\n",__func__,my_thread_pull->main);
	atomic_inc(&entry->mm->mm_users);
	f = filp_open(clone->exe_path, O_RDONLY | O_LARGEFILE | O_EXCL, 0);
	if (IS_ERR(f)) {
		printk("ERROR: error opening exe_path\n");
		return;
	}
	set_mm_exe_file(entry->mm, f);
	filp_close(f, NULL);
	entry->mm->start_stack = clone->stack_start;
	entry->mm->start_brk = clone->start_brk;
	entry->mm->brk = clone->brk;
	entry->mm->env_start = clone->env_start;
	entry->mm->env_end = clone->env_end;
	entry->mm->arg_start = clone->arg_start;
	entry->mm->arg_end = clone->arg_end;
	entry->mm->start_code = clone->start_code;
	entry->mm->end_code = clone->end_code;
	entry->mm->start_data = clone->start_data;
	entry->mm->end_data = clone->end_data;
	entry->mm->def_flags = clone->def_flags;

	__copy_task_comm(my_thread_pull->main, clone->exe_path);

	lock_task_sighand(my_thread_pull->main, &flags);
	my_thread_pull->main->tgroup_home_cpu = clone->tgroup_home_cpu;
	my_thread_pull->main->tgroup_home_id = clone->tgroup_home_id;
	my_thread_pull->main->tgroup_distributed = 1;
	unlock_task_sighand(my_thread_pull->main, &flags);
	
	//the main will be activated only when my_thread_pull->memory !=NULL
	my_thread_pull->memory = entry;
	wake_up_process(my_thread_pull->main);
	//printk("%s before calling create user thread\n",__func__);

	create_user_thread_for_distributed_process(clone, my_thread_pull);
}

static int clone_remote_thread(clone_request_t* clone, int inc)
{
	struct file* f;
	memory_t* memory = NULL;
	unsigned long flags;
	int ret;
	memory_t *entry;
	thread_pull_t *my_thread_pull;

	PSPRINTK("%s inc %d cpu %d id %d prev pid %d origin pid %d\n", __func__,
			inc, clone->tgroup_home_cpu, clone->tgroup_home_id,
			clone->prev_pid, clone->origin_pid);
retry:
	memory = find_memory_entry(clone->tgroup_home_cpu, clone->tgroup_home_id);
	if (memory) {
		PSPRINTK("%s memory found\n", __func__);
		if (inc) {
			atomic_inc(&(memory->pending_migration));
			/*
			int app = atomic_add_return(1, &(memory->pending_migration));
			if (app==57)
				atomic_set(&(memory->pending_migration),114);
			*/
		}
		if (memory->thread_pull) {
			return create_user_thread_for_distributed_process(clone,
									  memory->thread_pull);
		}
		//	printk("%s thread pull not ready yet\n", __func__);
		return -1;
	}


	PSPRINTK("%s memory not found\n", __func__);
	entry = (memory_t *)kmalloc(sizeof(memory_t), GFP_ATOMIC);
	BUG_ON(!entry);

	INIT_LIST_HEAD(&entry->list);
	entry->tgroup_home_cpu = clone->tgroup_home_cpu;
	entry->tgroup_home_id = clone->tgroup_home_id;
	entry->setting_up = 1;
	entry->thread_pull = NULL;
	atomic_set(&(entry->pending_migration),1);
	ret = add_memory_entry_with_check(entry);

	PSPRINTK("%s ret %d\n", __func__, ret);
	if (ret != 0) {
		kfree(entry);
		goto retry;
	}

	my_thread_pull = (thread_pull_t*)pop_data(
		(data_header_t**)&thread_pull_head, &thread_pull_head_lock);

	PSPRINTK("%s my_thread_pull %lx\n", __func__, my_thread_pull);
	if (!my_thread_pull) {
		struct work_struct* work = kmalloc(sizeof(struct work_struct),
				GFP_ATOMIC);
		if (work) {
			INIT_WORK(work, update_thread_pull);
			queue_work(clone_wq, work);
		}
		return -EAGAIN;
	}

	entry->thread_pull = my_thread_pull;
	entry->main = my_thread_pull->main;
	entry->mm = my_thread_pull->main->mm;
	atomic_inc(&entry->mm->mm_users);
	f = filp_open(clone->exe_path, O_RDONLY | O_LARGEFILE | O_EXCL, 0);
	if (IS_ERR(f)) {
		printk("%s: ERROR: error opening exe_path %s\n", __func__, clone->exe_path);
		return -1;
	}
	set_mm_exe_file(entry->mm, f);
	filp_close(f, NULL);
	entry->mm->start_stack = clone->stack_start;
	entry->mm->start_brk = clone->start_brk;
	entry->mm->brk = clone->brk;
	entry->mm->env_start = clone->env_start;
	entry->mm->env_end = clone->env_end;
	entry->mm->arg_start = clone->arg_start;
	entry->mm->arg_end = clone->arg_end;
	entry->mm->start_code = clone->start_code;
	entry->mm->end_code = clone->end_code;
	entry->mm->start_data = clone->start_data;
	entry->mm->end_data = clone->end_data;
	entry->mm->def_flags = clone->def_flags;

#undef INITIAL_VDSO_MODEL
#ifdef INITIAL_VDSO_MODEL
	// if popcorn_vdso is zero it should be initialized with the address provided by the home kernel
	if (entry->mm->context.popcorn_vdso == 0) {
		unsigned long popcorn_addr = clone->popcorn_vdso;
		struct page ** popcorn_pagelist = kzalloc(sizeof(struct page *) * (1 + 1), GFP_KERNEL);
		if (popcorn_pagelist == NULL) {
			pr_err("Failed to allocate vDSO pagelist!\n");
			ret = -ENOMEM;
			goto up_fail;
		}
		popcorn_pagelist[0] = alloc_pages(GFP_KERNEL | __GFP_ZERO, 0);
		if (!popcorn_pagelist[0]) {
			printk("%s: ERROR: alloc_pages failed for popcorn_vdso\n", __func__);
			ret = -ENOMEM;
			goto up_fail;
		}
		ret = install_special_mapping(
				entry->mm, popcorn_addr, PAGE_SIZE,
				VM_READ|VM_EXEC | VM_MAYREAD | VM_MAYWRITE | VM_MAYEXEC,
				popcorn_pagelist);
		if (!ret) {
			entry->mm->context.popcorn_vdso = (void *)popcorn_addr;
		} else {
			free_page((unsigned long)popcorn_pagelist[0]);
		}
	}
#else
	if (entry->mm->context.popcorn_vdso == 0)
		entry->mm->context.popcorn_vdso = clone->popcorn_vdso;
#endif

//up_fail:
	// popcorn_vdso cannot be different
	if (entry->mm->context.popcorn_vdso != clone->popcorn_vdso) {
		printk(KERN_ERR"%s: ERROR: popcorn_vdso entry:%p clone:%p\n",
				__func__, entry->mm->context.popcorn_vdso, clone->popcorn_vdso);
		BUG();
	}

	__copy_task_comm(my_thread_pull->main, clone->exe_path);

	lock_task_sighand(my_thread_pull->main, &flags);
	my_thread_pull->main->tgroup_home_cpu = clone->tgroup_home_cpu;
	my_thread_pull->main->tgroup_home_id = clone->tgroup_home_id;
	my_thread_pull->main->tgroup_distributed = 1;
	unlock_task_sighand(my_thread_pull->main, &flags);
	//the main will be activated only when my_thread_pull->memory !=NULL
	my_thread_pull->memory = entry;
	PSPRINTK("%s: wake up process\n", __func__);
	PSPRINTK("%s: wake up process %lx\n", __func__, my_thread_pull->main);
	PSPRINTK("%s: wake up process %d\n", __func__, my_thread_pull->main->pid);
	wake_up_process(my_thread_pull->main);
	//printk("%s before calling create user thread\n",__func__);
	return create_user_thread_for_distributed_process(clone, my_thread_pull);
}

///////////////////////////////////////////////////////////////////////////////
// handle clone
///////////////////////////////////////////////////////////////////////////////
static void try_create_remote_thread(struct work_struct *_work)
{
	clone_work_t* work = (clone_work_t*)_work;
	clone_request_t* request = work->request;
	const int delay_jiffies = 10;

	if (clone_remote_thread(request, 0) == 0 ) {
		pcn_kmsg_free_msg((struct pcn_kmsg_message *)request);
		kfree(work);
		return;
	}

	/* Put back the work */
	INIT_DELAYED_WORK((struct delayed_work*)work, try_create_remote_thread);
	queue_delayed_work(clone_wq, (struct delayed_work*)work, delay_jiffies);
}

static int handle_clone_request(struct pcn_kmsg_message* inc_msg)
{
	clone_request_t *request = (clone_request_t*)inc_msg;
	clone_work_t *work;
	int ret;

#if MIGRATION_PROFILE
	migration_start = ktime_get();
#endif
	ret = clone_remote_thread(request, 1);
	if (ret == 0) {
		pcn_kmsg_free_msg(inc_msg);
		return 0;
	}

	work = kmalloc(sizeof(*work), GFP_ATOMIC);
	BUG_ON(!work);

	work->request = request;
	if (ret == -EAGAIN) {
		INIT_WORK((struct work_struct *)work, clone_remote_thread_failed);
	} else {
		INIT_WORK((struct work_struct *)work, try_create_remote_thread);
	}
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
 * process_server_init
 * Start the process loop in a new kthread. (?)
 */
int __init process_server_init(void)
{
	uint16_t copy_cpu;

	printk(KERN_INFO"Popcorn with user data replication\n");

	/*
#ifndef SUPPORT_FOR_CLUSTERING
	_cpu = smp_processor_id();
#else
	_cpu = cpumask_first(cpu_present_mask);
#endif
	*/
	if (pcn_kmsg_get_node_ids(NULL, 0, &copy_cpu) == -1) {
		printk("ERROR process_server cannot initialize _cpu\n");
	} else {
		_cpu = copy_cpu;
#if 0 // beowulf
		_file_cpu = _cpu;
#endif
		printk(KERN_INFO"Popcorn started on cpu %d\n",_cpu);
	}

	/*
	 * Create a work queue so that we can do bottom side
	 * processing on data that was brought in by the
	 * communications module interrupt handlers.
	 */
	clone_wq = create_workqueue("clone_wq");
	exit_wq  = create_workqueue("exit_wq");
	exit_group_wq = create_workqueue("exit_group_wq");
	new_kernel_wq = create_workqueue("new_kernel_wq");

#if STATISTICS
	// TODO make sure to refactor the following
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

	/*
	 * Register handlers for kernels
	 */
	pcn_kmsg_register_callback(PCN_KMSG_TYPE_PROC_SRV_EXIT_PROCESS,
				   handle_exiting_process_notification);
	pcn_kmsg_register_callback(PCN_KMSG_TYPE_PROC_SRV_CREATE_PROCESS_PAIRING,
				   handle_process_pairing_request);
	pcn_kmsg_register_callback(PCN_KMSG_TYPE_PROC_SRV_CLONE_REQUEST,
				   handle_clone_request);
	pcn_kmsg_register_callback(PCN_KMSG_TYPE_PROC_SRV_THREAD_COUNT_REQUEST,
				   handle_remote_thread_count_request);
	pcn_kmsg_register_callback(PCN_KMSG_TYPE_PROC_SRV_THREAD_COUNT_RESPONSE,
				   handle_remote_thread_count_response);
	pcn_kmsg_register_callback(PCN_KMSG_TYPE_PROC_SRV_THREAD_GROUP_EXITED_NOTIFICATION,
				   handle_thread_group_exited_notification);
	pcn_kmsg_register_callback(PCN_KMSG_TYPE_PROC_SRV_BACK_MIG_REQUEST,
				   handle_back_migration);
	pcn_kmsg_register_callback(PCN_KMSG_TYPE_PROC_SRV_CREATE_THREAD_PULL,
				   handle_thread_pull_creation);


	pcn_kmsg_register_callback(PCN_KMSG_TYPE_PROC_SRV_NEW_KERNEL,
				   handle_new_kernel);
	pcn_kmsg_register_callback(PCN_KMSG_TYPE_PROC_SRV_NEW_KERNEL_ANSWER,
				   handle_new_kernel_answer);

	file_handler_init();

	return 0;
}
