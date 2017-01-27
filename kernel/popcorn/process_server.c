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
#include <linux/ptrace.h>

#include <asm/mmu_context.h>

#define  NSIG 32

#include <linux/cpu_namespace.h>
#include <linux/popcorn_cpuinfo.h>
#include <linux/process_server.h>

#include <process_server_arch.h>
#include "stat.h"
#include "internal.h"
#include "vma_server.h"
#include <popcorn/bundle.h>

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

#if 0 // beowulf
int _cpu = 0;		// This is for any Popcorn Linux server

int _file_cpu = 1;	// This is for the Popcorn Linux file server
#endif

///////////////////////////////////////////////////////////////////////////////
// Marina's data stores (linked lists)
///////////////////////////////////////////////////////////////////////////////
LIST_HEAD(_count_head);
DEFINE_SPINLOCK(_count_head_lock);

LIST_HEAD(_vma_ack_head);
DEFINE_SPINLOCK(_vma_ack_head_lock);
/*
 * Functions to manipulate memory list
 * head: _memory_head
 * lock: _memory_head_lock
 */
LIST_HEAD(_memory_head);
DEFINE_SPINLOCK(_memory_head_lock);

void add_memory_entry(memory_t* entry)
{
	unsigned long flags;

	BUG_ON(!entry);

	spin_lock_irqsave(&_memory_head_lock, flags);
	list_add_tail(&entry->list, &_memory_head);
	spin_unlock_irqrestore(&_memory_head_lock, flags);
}

int add_memory_entry_with_check(memory_t* entry)
{
	memory_t *m;
	unsigned long flags;

	BUG_ON(!entry);

	spin_lock_irqsave(&_memory_head_lock, flags);
	list_for_each_entry(m, &_memory_head, list) {
		if ((m->tgroup_home_cpu == entry->tgroup_home_cpu
			  && m->tgroup_home_id == entry->tgroup_home_id)) {
			spin_unlock_irqrestore(&_memory_head_lock, flags);
			return -1;
		}
	}
	list_add_tail(&entry->list, &_memory_head);
	spin_unlock_irqrestore(&_memory_head_lock,flags);

	return 0;
}

memory_t* find_memory_entry(int cpu, int id)
{
	memory_t *m = NULL;
	memory_t *found = NULL;
	unsigned long flags;

	spin_lock_irqsave(&_memory_head_lock, flags);
	list_for_each_entry(m, &_memory_head, list) {
		if (m->tgroup_home_cpu == cpu && m->tgroup_home_id == id) {
#ifdef CHECK_FOR_DUPLICATES
			if (found) {
				printk(KERN_ERR"%s: duplicates in list %s %s (cpu %d id %d)\n",
						__func__, found->path, m->path, cpu, id);
			}
			found = m;
#else
			found = m;
			break;
#endif
		}
	}
	spin_unlock_irqrestore(&_memory_head_lock, flags);

	return found;
}

int dump_memory_entries(memory_t * list[], int num, int *written)
{
	memory_t *m = NULL;
	int i = 0;
	bool more = false;
	unsigned long flags;

	spin_lock_irqsave(&_memory_head_lock,flags);
	list_for_each_entry(m, &_memory_head, list) {
		if (i >= num) {
			more = true;
			break;
		}
		list[i] = m;
		i++;
	}
	spin_unlock_irqrestore(&_memory_head_lock,flags);

	if (written)
		*written = i;

	return more;
}

void remove_memory_entry(memory_t* entry)
{
	unsigned long flags;

	BUG_ON(!entry);

	spin_lock_irqsave(&_memory_head_lock,flags);
	list_del_init(&entry->list);
	spin_unlock_irqrestore(&_memory_head_lock,flags);
}


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
 * Create a kernel thread - required for process server to create a
 * kernel thread from user context.
 */
pid_t kernel_thread_popcorn(int (*fn)(void *), void *arg)
{
	const unsigned long flags = CLONE_THREAD | CLONE_SIGHAND | SIGCHLD;
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


static int count_remote_thread_members(int tgroup_home_cpu, int tgroup_home_id, memory_t *memory)
{
	count_answers_t *data;
	remote_thread_count_request_t *req;
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

	req = kmalloc(sizeof(*req), GFP_ATOMIC);
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
			//s = pcn_kmsg_send(i, (struct pcn_kmsg_message *)(&request));
			int sent = pcn_kmsg_send_long(i, req, sizeof(*req));
			if (sent != -1 ) {
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
	//printk("%s data->count is %d", __func__, data->count);

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
	new_kernel_work_answer_t *work = (new_kernel_work_answer_t *)_work;
	new_kernel_answer_t *answer = work->answer;
	memory_t *memory = work->memory;

	if (answer->header.from_cpu == answer->tgroup_home_cpu) {
		down_write(&memory->mm->mmap_sem);
		//	printk("%s answer->vma_operation_index %d NR_CPU %d\n", __func__, answer->vma_operation_index, MAX_KERNEL_IDS);
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
static int handle_new_kernel_answer(struct pcn_kmsg_message *inc_msg)
{
	new_kernel_answer_t *answer = (new_kernel_answer_t *)inc_msg;
	memory_t *memory = find_memory_entry(answer->tgroup_home_cpu,
					    answer->tgroup_home_id);

	PSNEWTHREADPRINTK("received new kernel answer\n");
	//printk("%s: %d\n", __func__, answer->vma_operation_index);
	if (memory != NULL) {
		new_kernel_work_answer_t *work = kmalloc(sizeof(*work), GFP_ATOMIC);
		BUG_ON(!work);
		work->answer = answer;
		work->memory = memory;
		INIT_WORK((struct work_struct *)work, process_new_kernel_answer);
		queue_work(new_kernel_wq, (struct work_struct *)work);
	} else {
		printk("%s: ERROR: received an answer for new kernel but memory_t not present cpu %d id %d\n",
				__func__, answer->tgroup_home_cpu, answer->tgroup_home_id);
		pcn_kmsg_free_msg(inc_msg);
	}

	return 1;
}

static void process_new_kernel(struct work_struct *_work)
{
	new_kernel_work_t *work = (new_kernel_work_t *)_work;
	new_kernel_t *req = work->request;
	memory_t *memory;
	new_kernel_answer_t *answer = kmalloc(sizeof(*answer), GFP_ATOMIC);
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

		if (get_nid() == req->tgroup_home_cpu) {
			down_read(&memory->mm->mmap_sem);
			answer->vma_operation_index = memory->mm->vma_operation_index;
			up_read(&memory->mm->mmap_sem);
		}
	} else {
		PSPRINTK("memory not present\n");
		memset(answer->my_set, 0, sizeof(answer->my_set));
	}

	answer->header.type = PCN_KMSG_TYPE_PROC_SRV_NEW_KERNEL_RESPONSE;
	answer->header.prio = PCN_KMSG_PRIO_NORMAL;

	answer->tgroup_home_cpu = req->tgroup_home_cpu;
	answer->tgroup_home_id = req->tgroup_home_id;

	pcn_kmsg_send_long(req->header.from_cpu, answer, sizeof(*answer));

	kfree(answer);
	kfree(work);
	pcn_kmsg_free_msg(req);
}

static int handle_new_kernel(struct pcn_kmsg_message *inc_msg)
{
	new_kernel_t *req= (new_kernel_t *)inc_msg;
	new_kernel_work_t *work;

	work = kmalloc(sizeof(new_kernel_work_t), GFP_ATOMIC);

	if (work) {
		work->request = req;
		INIT_WORK( (struct work_struct *)work, process_new_kernel);
		queue_work(new_kernel_wq, (struct work_struct *)work);
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
static int exit_distributed_process(memory_t *mm_data, int flush)
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

	if (mm_data->alive == 0 && !is_last_thread_in_local_group && atomic_read(&(mm_data->pending_migration)) ==0) {
		printk("%s: ERROR: mm_data->alive is 0 but there are alive threads (id %d, cpu %d)\n", __func__, current->tgroup_home_id, current->tgroup_home_cpu);
		return 0;
	}

	if (mm_data->alive == 0  && atomic_read(&(mm_data->pending_migration)) ==0) {

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

		if (atomic_read(&(mm_data->pending_migration)) != 0)
			printk(KERN_ALERT"%s: ERROR pending migration when cleaning memory (id %d, cpu %d)\n", __func__, current->tgroup_home_id, current->tgroup_home_cpu);

		/*
		 * Empty unused shadow thread pool

		shadow_thread_t *my_shadow = NULL;
		my_shadow = (shadow_thread_t *)pop_data((data_header_t **)&(my_thread_pool->threads),
							 &(my_thread_pool->spinlock));

		while (my_shadow){
			my_shadow->thread->distributed_exit = EXIT_THREAD;
			wake_up_process(my_shadow->thread);
			kfree(my_shadow);
			my_shadow = (shadow_thread_t *)pop_data((data_header_t **)&(my_thread_pool->threads),
								 &(my_thread_pool->spinlock));
		}
		*/
		remove_memory_entry(mm_data);
		mmput(mm_data->mm);
		kfree(mm_data);

#if STATISTICS
		print_popcorn_stat();
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
							current->tgroup_home_id, mm_data);
		/* Ok this is complicated.
		 * If count is zero = > all the threads of my process went through this exit function (all task->distributed_exit ==1 or
		 * there are no more tasks of this process around).
		 * Dying tasks that did not see count ==0 saved a copy of the mapping. Someone should notice their kernels that now they can erase it.
		 * I can be the one, however more threads can be concurrently in this exit function on different kernels =>
		 * each one of them can see the count ==0 => more than one "erase mapping message" can be sent.
		 * If count ==0 I check if I already receive a "erase mapping message" and avoid to send another one.
		 * This check does not guarantee that more than one "erase mapping message" cannot be sent (in some executions it is inevitable) =>
		 * just be sure to not call more than one mmput one the same mapping!!!
		 */
		if (count == 0) {
			mm_data->alive = 0;
			if (status != EXIT_PROCESS) {
				_remote_cpu_info_list_t *objPtr, *tmpPtr;
				thread_group_exited_notification_t *exit_notification = kmalloc(sizeof(*exit_notification), GFP_ATOMIC);

				PSPRINTK("%s: This is the last thread of process (id %d, cpu %d) in the system, "
					 "sending an erase mapping message!\n", __func__, current->tgroup_home_id, current->tgroup_home_cpu);
				exit_notification->header.type = PCN_KMSG_TYPE_PROC_SRV_THREAD_GROUP_EXITED_NOTIFICATION;
				exit_notification->header.prio = PCN_KMSG_PRIO_NORMAL;
				exit_notification->tgroup_home_cpu = current->tgroup_home_cpu;
				exit_notification->tgroup_home_id = current->tgroup_home_id;
				// the list does not include the current processor group descirptor (TODO)
				list_for_each_entry_safe(objPtr, tmpPtr, &rlist_head, cpu_list_member) {
					i = objPtr->_data._processor;
					pcn_kmsg_send_long(i, (struct pcn_kmsg_long_message *)(exit_notification), sizeof(thread_group_exited_notification_t));

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
		if (atomic_read(&(mm_data->pending_migration)) ==0)
			mm_data->alive = 0;
	}

	if ((!is_last_thread_in_local_group || count != 0) && status == EXIT_PROCESS) {
		printk("%s: ERROR: received an exit process but is_last_thread_in_local_group id %d and count is %d\n ",
			   __func__, is_last_thread_in_local_group, count);
	}
	return 0;
}


///////////////////////////////////////////////////////////////////////////////
// main for distributed kernel thread (?)
///////////////////////////////////////////////////////////////////////////////
#define GET_UNMAP_IF_HOME(task, mm_data) \
({ if (task->tgroup_home_cpu != get_nid()) {\
	if (mm_data->mm->distribute_unmap == 0) \
		printk(KERN_ALERT"%s: GET_UNMAP_IF_HOME: ERROR: value was already 0, check who is the older.\n", __func__); \
	mm_data->mm->distribute_unmap = 0; \
	} \
})
#define PUT_UNMAP_IF_HOME(task, mm_data) \
({ if (task->tgroup_home_cpu != get_nid()) {\
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

	printk(KERN_INFO"%s: 0x%p %d %d\n", __func__,
			current, current->pid, current->tgid);

	// I'm a helper.
	if (current->main) return 0;

	// Remote proxy
	if (current->executing_for_remote) {
		// TODO: Send the result to the home
		return 0;
	}

	if (current->pid != current->tgid) return 0;

	printk("%s: Stopping 0x%p %d %d\n", __func__,
			current, current->pid, current->tgid);

	memory->helper_stop = true;
	wake_up_process(memory->helper);
	put_task_struct(memory->helper);

	if (memory->shadow_spawner) {
		complete(&memory->spawn_egg);
		put_task_struct(memory->shadow_spawner);
	}

	return 0;
}


///////////////////////////////////////////////////////////////////////////////
// exit group notification
///////////////////////////////////////////////////////////////////////////////
static void process_exit_group_notification(struct work_struct *_work)
{
	exit_group_work_t *work = (exit_group_work_t *)_work;
	thread_group_exited_notification_t *msg = work->request;
	unsigned long flags;

	memory_t *memory = find_memory_entry(msg->tgroup_home_cpu,
					      msg->tgroup_home_id);
	if (!memory)
		goto out_free;

	while (memory->helper == NULL)
		schedule();

	lock_task_sighand(memory->helper, &flags);
	memory->helper->distributed_exit = EXIT_PROCESS;
	unlock_task_sighand(memory->helper, &flags);

	memory->helper_stop = true;
	wake_up_process(memory->helper);
	put_task_struct(memory->helper);

	if (memory->shadow_spawner) {
		complete(&memory->spawn_egg);
		put_task_struct(memory->shadow_spawner);
	}

out_free:
	pcn_kmsg_free_msg(msg);
	kfree(work);
}

static void process_exiting_process_notification(struct work_struct *_work)
{
	exit_work_t *work = (exit_work_t *)_work;
	exiting_process_t *msg = work->request;

	unsigned int source_cpu = msg->header.from_cpu;
	struct task_struct *tsk;

	tsk = find_task_by_vpid(msg->prev_pid);

	if (tsk && tsk->next_pid == msg->my_pid
		&& tsk->next_cpu == source_cpu
	    && tsk->represents_remote == 1) {

		// TODO: Handle return values
		restore_thread_info(tsk, &msg->arch);

		tsk->group_exit = msg->group_exit;
		tsk->distributed_exit_code = msg->code;
#if MIGRATE_FPU
		// TODO: Handle return values
		restore_fpu_info(tsk, &msg->arch);
#endif
		wake_up_process(tsk);
	} else {
		printk(KERN_ALERT"%s: ERROR: task not found."
				"Impossible to kill shadow (pid %d cpu %d)\n",
				__func__, msg->my_pid, source_cpu);
	}

	pcn_kmsg_free_msg(msg);
	kfree(work);
}

static int handle_thread_group_exited_notification(struct pcn_kmsg_message *inc_msg)
{
	exit_group_work_t *work;
	thread_group_exited_notification_t *req =
		(thread_group_exited_notification_t *)inc_msg;

	work = kmalloc(sizeof(exit_group_work_t), GFP_ATOMIC);

	if (work) {
		work->request = req;
		INIT_WORK((struct work_struct *)work,
			   process_exit_group_notification);
		queue_work(exit_group_wq, (struct work_struct *) work);
	}
	return 1;
}

static int handle_exiting_process_notification(struct pcn_kmsg_message *inc_msg)
{
	exit_work_t *work;
	exiting_process_t *request = (exiting_process_t *)inc_msg;

	work = kmalloc(sizeof(exit_work_t), GFP_ATOMIC);
	if (work) {
		work->request = request;
		INIT_WORK((struct work_struct *)work,
			   process_exiting_process_notification);
		queue_work(exit_wq, (struct work_struct *)work);
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
static int handle_process_pairing_request(struct pcn_kmsg_message *_msg)
{
	create_process_pairing_t *req = (create_process_pairing_t *)_msg;
	unsigned int source_cpu = req->header.from_cpu;
	struct task_struct *task;

	task = find_task_by_vpid(req->your_pid);
	if (task == NULL || task->represents_remote == 0) {
		pcn_kmsg_free_msg(req);
		return -ESRCH;
	}

	PSPRINTK("Pairing local:  %d --> %d at %d\n",
			task->pid, source_cpu, req->my_pid);
	task->next_cpu = source_cpu;
	task->next_pid = req->my_pid;
	task->executing_for_remote = 0;

	pcn_kmsg_free_msg(req);
	return 0;
}

static int handle_remote_thread_count_response(struct pcn_kmsg_message *inc_msg)
{
	remote_thread_count_response_t *msg =
			(remote_thread_count_response_t *)inc_msg;
	count_answers_t *data =
			find_count_entry(msg->tgroup_home_cpu, msg->tgroup_home_id);
	unsigned long flags;
	struct task_struct *to_wake = NULL;

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

static void process_count_request(struct work_struct *_work)
{
	count_work_t *work = (count_work_t *)_work;
	remote_thread_count_request_t *request = work->request;
	remote_thread_count_response_t *response;
	memory_t *memory;

	PSPRINTK("%s: entered - cpu{%d}, id{%d}\n", __func__,
			request->tgroup_home_cpu, request->tgroup_home_id);

	response = kmalloc(sizeof(*response), GFP_ATOMIC);
	BUG_ON(!response);
	response->count = 0;

	/* This is needed to know if the requesting kernel has to save the
	 * mapping or send the group dead message.
	 * If there is at least one alive thread of the process in the system
	 * the mapping must be saved.
	 * I count how many threads there are but actually I can stop when
	 * I know that there is one.
	 * If there are no more threads in the system, a group dead message
	 * should be sent by at least one kernel.
	 * I do not need to take the sighand lock (used to set
	 * task->distributed_exit = 1) because:
	 * -- count remote thread is called AFTER set task->distributed_exit = 1
	 * -- if I am here the last thread of the process in the requesting
	 *  kernel already set his flag distributed_exit to 1
	 * -- two things can happend if the last thread of the process is
	 *  in this kernel and it is dying too:
	 * -- 1. set its flag before I check it => I send 0 => the other
	 *  kernel will send the message
	 * -- 2. set its flag after I check it => I send 1 => I will send
	 *  the message
	 * Is important to not take the lock so everything can be done
	 * in the messaging layer without fork another kthread.
	 */

	memory = find_memory_entry(
			request->tgroup_home_cpu, request->tgroup_home_id);
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
			(struct pcn_kmsg_long_message *)response,
			sizeof(*response));

	pcn_kmsg_free_msg(request);
	kfree(response);
	kfree(work);
}

static int handle_remote_thread_count_request(struct pcn_kmsg_message *inc_msg)
{
	count_work_t *work;
	remote_thread_count_request_t *request =
			(remote_thread_count_request_t *)inc_msg;

	work = kmalloc(sizeof(*work), GFP_ATOMIC);
	BUG_ON(!work);

	work->request = request;
	INIT_WORK( (struct work_struct *)work, process_count_request);
	queue_work(exit_wq, (struct work_struct *)work);

	return 1;
}

///////////////////////////////////////////////////////////////////////////////
// synch migrations (?)
///////////////////////////////////////////////////////////////////////////////
void synchronize_migrations(int tgroup_home_cpu, int tgroup_home_id )
{
	memory_t *memory = NULL;

	memory = find_memory_entry(tgroup_home_cpu, tgroup_home_id);
	if (!memory) {
		printk("%s: ERROR: no memory_t found (cpu %d id %d)\n",
				__func__, tgroup_home_cpu, tgroup_home_id);
		BUG();
		return;
	}

	atomic_dec(&(memory->pending_back_migration));
	while (atomic_read(&(memory->pending_back_migration)) != 0) {
		msleep(1);
	}
}

///////////////////////////////////////////////////////////////////////////////
// handling back migration
///////////////////////////////////////////////////////////////////////////////
static int handle_back_migration(struct pcn_kmsg_message *inc_msg)
{
	back_migration_request_t *req = (back_migration_request_t *) inc_msg;
	memory_t *memory = NULL;
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
	memory_t *entry = NULL;
	unsigned long flags;

	PSPRINTK("MORTEEEEEE-Process_server_task_exit_notification - pid{%d}\n",
			tsk->pid);

	if (tsk->distributed_exit ==EXIT_ALIVE) {
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
			exiting_process_t *msg = (exiting_process_t *) kmalloc(
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
							    (struct pcn_kmsg_long_message *) msg,
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
	create_process_pairing_t *msg;
	int tx_ret = -1;

	msg = kmalloc(sizeof(*msg), GFP_KERNEL);
	BUG_ON(!msg);

	// Notify remote cpu of pairing between current task and remote
	// representative task.
	msg->header.type = PCN_KMSG_TYPE_PROC_SRV_CREATE_PROCESS_PAIRING;
	msg->header.prio = PCN_KMSG_PRIO_NORMAL;
	msg->your_pid = remote_pid;
	msg->my_pid = my_pid;

	PSPRINTK("Pairing remote: %d --> %d at %d\n",
			my_pid, remote_pid, remote_cpu);

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
static int do_back_migration(struct task_struct *tsk, int dst_cpu,
			     struct pt_regs *regs, void __user *uregs)
{
	back_migration_request_t *req;
	unsigned long flags;
	int ret;

	might_sleep();

	req = kmalloc(sizeof(*req), GFP_KERNEL);
	BUG_ON(req);

	PSPRINTK("%s\n", __func__);

	//printk("%s entered dst{%d}\n", __func__, dst_cpu);
	req->header.type = PCN_KMSG_TYPE_PROC_SRV_BACK_MIGRATE;
	req->header.prio = PCN_KMSG_PRIO_NORMAL;

	req->tgroup_home_cpu = tsk->tgroup_home_cpu;
	req->tgroup_home_id = tsk->tgroup_home_id;
	req->placeholder_pid = tsk->pid;
	req->placeholder_tgid = tsk->tgid;
	req->prev_pid = tsk->prev_pid;
	req->personality = tsk->personality;

	req->origin_pid = tsk->origin_pid;
	req->remote_blocked = tsk->blocked;
	req->remote_real_blocked = tsk->real_blocked;
	req->remote_saved_sigmask = tsk->saved_sigmask;
	req->remote_pending = tsk->pending;
	req->sas_ss_sp = tsk->sas_ss_sp;
	req->sas_ss_size = tsk->sas_ss_size;
	memcpy(req->action, tsk->sighand->action, sizeof(req->action));

	// TODO: Handle return value
	save_thread_info(tsk, regs, &req->arch, uregs);

#if MIGRATE_FPU
	// TODO: Handle return value
	save_fpu_info(tsk, &req->arch);
#endif

	lock_task_sighand(tsk, &flags);

	tsk->represents_remote = 1;
	tsk->next_cpu = tsk->prev_cpu;
	tsk->next_pid = tsk->prev_pid;
	tsk->executing_for_remote = 0;
	unlock_task_sighand(tsk, &flags);

	ret = pcn_kmsg_send_long(dst_cpu, req, sizeof(*req));

#if MIGRATION_PROFILE
	migration_end = ktime_get();
	printk(KERN_ERR"Time for x86->arm back migration - x86 side: %ld ns\n",
		(unsigned long)ktime_to_ns(ktime_sub(migration_end, migration_start)));
#endif

	kfree(req);
	return ret;
}


/*
 * Main loop for in-kernel request handler
 */
/*
static int helper_thread_main_(void)
{
	int count = 10;
	memory_t *memory = current->memory;
	might_sleep();

	printk("%s: Started pid=%d\n", __func__, current->pid);

	while (!memory->helper_stop && count > 0) {
		__set_task_state(current, TASK_UNINTERRUPTIBLE);
		schedule_timeout(HZ * 1);
		printk("%s: continues 0x%p %d\n", __func__, current, count--);
		__set_task_state(current, TASK_RUNNING);
	}
	printk("%s: Exit helper %d\n", __func__, current->pid);

	return 0;
}
*/

static int helper_thread_main(void)
{
	memory_t *memory = current->memory;

	might_sleep();

	printk("%s: Started pid=%d\n", __func__, current->pid);

	// TODO: Need to explore how this has to be used
	// TODO: Exit the thread
	while (!memory->helper_stop) {
		int flush = 0;

		/*
		__set_task_state(current, TASK_UNINTERRUPTIBLE);
		schedule_timeout(HZ * 1);
		printk("%s: continues 0x%p\n", __func__, current);
		__set_task_state(current, TASK_RUNNING);
		continue;
		*/

		while (current->distributed_exit != EXIT_ALIVE) {
			flush = exit_distributed_process(memory, flush);
			msleep(1000);
		}
		while (memory->operation != VMA_OP_NOP &&
		       memory->mm->thread_op == current) {
			struct file *f;
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
				(memory->operation != VMA_OP_NOP &&
				 memory->mm->thread_op == current)) {
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


/*
 * Send a message to <dst_cpu> for migrating a task <task>.
 * This function will ask the remote cpu to create a thread to host task.
 * It returns -1 in error case.
 */
static int create_helper_thread_from_user(void *_memory)
{
	memory_t *memory = (memory_t *)_memory;

	might_sleep();

	BUG_ON(memory != current->memory);

	current->main = 1;	// Yes, i'm a helper

	helper_thread_main();

	return 0;
}


static int __request_clone_remote(int dst_nid, struct task_struct *tsk,
		struct pt_regs *regs, void __user *uregs)
{
	char *rpath, path[512];
	clone_request_t *req;
	int sent;

	might_sleep();

	req = kmalloc(sizeof(*req), GFP_KERNEL);
	BUG_ON(!req);

	/* Build request */
	req->header.type = PCN_KMSG_TYPE_PROC_SRV_MIGRATE;
	req->header.prio = PCN_KMSG_PRIO_NORMAL;

	req->tgroup_home_cpu = get_nid();
	req->tgroup_home_id = tsk->tgid;

	// struct mm_struct --------------------------------------------------------
	rpath = d_path(&tsk->mm->exe_file->f_path, path, sizeof(path));
	if (IS_ERR(rpath)) {
		printk("%s: exe binary path is too long.\n", __func__);
		kfree(req);
		return -ESRCH;
	} else {
		strncpy(req->exe_path, rpath, sizeof(req->exe_path));
		// printk("%s: exe_path=%s\n", __func__, rpath);
	}

	req->stack_start = tsk->mm->start_stack;
	req->start_brk = tsk->mm->start_brk;
	req->brk = tsk->mm->brk;
	req->env_start = tsk->mm->env_start;
	req->env_end = tsk->mm->env_end;
	req->arg_start = tsk->mm->arg_start;
	req->arg_end = tsk->mm->arg_end;
	req->start_code = tsk->mm->start_code;
	req->end_code = tsk->mm->end_code;
	req->start_data = tsk->mm->start_data;
	req->end_data = tsk->mm->end_data;
	req->def_flags = tsk->mm->def_flags;
	req->popcorn_vdso = tsk->mm->context.popcorn_vdso;

	// struct tsk_struct ------------------------------------------------------
	req->placeholder_pid = tsk->pid;
	req->placeholder_tgid = tsk->tgid;
	req->personality = tsk->personality;

	if (tsk->prev_pid == -1)
		req->origin_pid = tsk->pid;
	else
		req->origin_pid = tsk->origin_pid;

	req->remote_blocked = tsk->blocked;
	req->remote_real_blocked = tsk->real_blocked;
	req->remote_saved_sigmask = tsk->saved_sigmask;
	req->remote_pending = tsk->pending;
	req->sas_ss_sp = tsk->sas_ss_sp;
	req->sas_ss_size = tsk->sas_ss_size;
	memcpy(req->action, tsk->sighand->action, sizeof(req->action));

	save_thread_info(tsk, regs, &req->arch, uregs);

#if MIGRATE_FPU
	save_fpu_info(tsk, &req->arch);
#endif

	sent = pcn_kmsg_send_long(dst_nid, req, sizeof(*req));
	kfree(req);

	return sent;
}

static memory_t *__alloc_memory_struct(int home_cpu, int home_id)
{
	memory_t *m = kzalloc(sizeof(*m), GFP_KERNEL);
	BUG_ON(!m);

	INIT_LIST_HEAD(&m->list);

	m->tgroup_home_cpu = home_cpu;
	m->tgroup_home_id = home_id;
	m->alive = 1;
	m->helper = NULL;
	atomic_set(&m->pending_migration, 0);
	atomic_set(&m->pending_back_migration, 0);
	m->operation = VMA_OP_NOP;
	m->waiting_for_main = NULL;
	m->waiting_for_op = NULL;
	m->arrived_op = 0;
	m->my_lock = 0;

	m->helper = NULL;
	m->helper_stop = false;

	m->shadow_spawner = NULL;
	INIT_LIST_HEAD(&m->shadow_eggs);
	spin_lock_init(&m->shadow_eggs_lock);
	init_completion(&m->spawn_egg);
	atomic_set(&m->pending_migration, 0);

	init_rwsem(&m->kernel_set_sem);
	//memset(m->kernel_set, 0x00, sizeof(m->kernel_set));
	m->kernel_set[get_nid()] = 1;

	INIT_LIST_HEAD(&m->pages);
	spin_lock_init(&m->pages_lock);

	printk(KERN_INFO"%s: at 0x%p for %d / %d\n",
			__func__, m, home_cpu, home_id);

	return m;
}

int do_migration(struct task_struct *tsk, int dst_nid,
		struct pt_regs *regs, void __user *uregs)
{
	int sent;
	unsigned long flags;
	bool create_helper = false;
	memory_t *memory;

	might_sleep();

	/* I use siglock to coordinate the thread group.
	 */
	lock_task_sighand(tsk, &flags);

	/*
	 * This process is becoming a distributed one if it was not already.
	 * The first migrating thread has to create the memory entry to
	 * handle page requests, and fork the main kernel thread of this process.
	 */
	if (tsk->tgroup_distributed == 0) {
		struct task_struct *thread;

		for_each_thread(tsk, thread) {
			thread->tgroup_home_cpu = get_nid();
			thread->tgroup_home_id = tsk->tgid;
			thread->tgroup_distributed = 1;
		};

		init_rwsem(&tsk->mm->distribute_sem);
		tsk->mm->distr_vma_op_counter = 0;
		tsk->mm->was_not_pushed = 0;
		tsk->mm->thread_op = NULL;
		tsk->mm->vma_operation_index = 0;
		tsk->mm->distribute_unmap = 1;

		memory = __alloc_memory_struct(get_nid(), tsk->tgid);
		memory->mm = tsk->mm;
		atomic_inc(&tsk->mm->mm_users);

		add_memory_entry_out(memory, dst_nid);

		create_helper = true;

		printk("%s: task distributed. cpu=%d, id=%d\n", __func__,
				tsk->tgroup_home_cpu, tsk->tgroup_home_id);
	} else {
		memory = find_memory_entry_out(
				tsk->tgroup_home_cpu, tsk->tgroup_home_id);
	}
	BUG_ON(!memory);
	tsk->memory = memory;
	tsk->represents_remote = 1;
	unlock_task_sighand(tsk, &flags);

	if (create_helper) {
		pid_t pid = kernel_thread_popcorn(
				create_helper_thread_from_user, memory);
		memory->helper = find_task_by_vpid(pid);
		get_task_struct(memory->helper);
	}

	sent = __request_clone_remote(dst_nid, tsk, regs, uregs);

#if MIGRATION_PROFILE
	migration_end = ktime_get();
	printk(KERN_ERR"Time for x86->arm migration - x86 side: %ld ns\n",
		(unsigned long)ktime_to_ns(ktime_sub(migration_end, migration_start)));
#endif

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
int process_server_do_migration(struct task_struct *task, int dst_nid,
				struct pt_regs *regs, void __user *uregs)
{
	int ret = 0;

	PSPRINTK(KERN_INFO"%s: current: pid=%d, tgid=%d "
			"task: tgroup_home_id=%d, tgroup_home_cpu=%d\n",
			__func__, current->pid, current->tgid,
			task->tgroup_home_id, task->tgroup_home_cpu);

	if (task->prev_cpu == dst_nid) {
		ret = do_back_migration(task, dst_nid, regs, uregs);
	} else {
		ret = do_migration(task, dst_nid, regs, uregs);
	}

	return ret;
}


struct shadow_main_param {
	struct completion start;
};

int shadow_main(void *_args)
{
	struct shadow_main_param *param = _args;
	memory_t *memory = current->memory;

    PSPRINTK("%s: start %d at 0x%p\n", __func__, current->pid, current);

	__set_task_state(current, TASK_UNINTERRUPTIBLE);
	complete(&param->start);
	schedule();

	//---------------- Attached here ---------------------

	PSPRINTK("%s: resume %d at 0x%p\n", __func__, current->pid, current);

	current->distributed_exit = EXIT_ALIVE;

	// Notify of PID/PID pairing.
	process_server_notify_delegated_subprocess_starting(
			current->pid, current->prev_pid, current->prev_cpu);

	memory->alive = 1;

	// TODO: Handle return values
	update_thread_info(current);

#if MIGRATE_FPU
	// TODO: Handle return values
	update_fpu_info(current);
#endif

	//to synchronize migrations...
	atomic_dec(&memory->pending_migration);

	kfree(param);
	return 0;	/* Returning from here will start the user thread run */
}


///////////////////////////////////////////////////////////////////////////////
// thread specific handling
///////////////////////////////////////////////////////////////////////////////
static int spawn_shadow_thread(clone_request_t *req)
{
	struct task_struct *tsk;
	struct shadow_main_param *param = kmalloc(sizeof(*param), GFP_KERNEL);
	const unsigned long flags = CLONE_THREAD | CLONE_SIGHAND | SIGCHLD;
	//int ret;
	pid_t pid;

	init_completion(&param->start);

	pid = kernel_thread(shadow_main, param, flags);
	if (pid < 0) {
		kfree(param);
		return -EAGAIN;
	}
	tsk = find_task_by_vpid(pid);
	get_task_struct(tsk);

	printk("%s: %d %p\n", __func__, pid, tsk);

	wait_for_completion(&param->start);	/* Make sure the shadow is sleeping */

	__rename_task_comm(tsk, req->exe_path);

	tsk->distributed_exit = EXIT_NOT_ACTIVE;
	tsk->prev_cpu = req->header.from_cpu;
	tsk->prev_pid = req->placeholder_pid;
	tsk->personality = req->personality;

	tsk->flags &= ~(PF_KTHREAD | PF_RANDOMIZE);	// Convert to user
	initialize_thread_retval(tsk, 0);

	// set thread info
	// TODO: Handle return values
	restore_thread_info(tsk, &req->arch);

	/*
	tsk->origin_pid = req->origin_pid;
	sigorsets(&tsk->blocked, &tsk->blocked, &req->remote_blocked) ;
	sigorsets(&tsk->real_blocked, &tsk->real_blocked, &req->remote_real_blocked);
	sigorsets(&tsk->saved_sigmask, &tsk->saved_sigmask, &req->remote_saved_sigmask);
	tsk->pending = req->remote_pending;
	tsk->sas_ss_sp = req->sas_ss_sp;
	tsk->sas_ss_size = req->sas_ss_size;
	memcpy(tsk->sighand->action, req->action, sizeof(req->action));
	*/

#if MIGRATE_FPU
	restore_fpu_info(tsk, &req->arch);
#endif

#if MIGRATION_PROFILE
	migration_end = ktime_get();
	printk(KERN_ERR "Time for arm->x86 migration - x86 side: %ld ns\n",
			ktime_to_ns(ktime_sub(migration_end, migration_start)));
#endif

	printk("####### MIGRATED - pid %u@%d to %u@%d\n",
			(unsigned int)tsk->prev_pid, tsk->prev_cpu, tsk->pid, get_nid());
	printk("%s: pc %lx\n", __func__, (&req->arch)->migration_pc);
	printk("%s: ip %lx\n", __func__, task_pt_regs(tsk)->ip);
	printk("%s: sp %lx\n", __func__, task_pt_regs(tsk)->sp);
	printk("%s: bp %lx\n", __func__, task_pt_regs(tsk)->bp);

	tsk->executing_for_remote = 1;
	wake_up_process(tsk);
	put_task_struct(tsk);

	return 0;
}


static void __kick_shadow_spawner(memory_t *memory, clone_work_t *work)
{
	// Utilize the list_head in work_struct
	struct list_head *entry = &((struct work_struct *)work)->entry;

	INIT_LIST_HEAD(entry);
	spin_lock(&memory->shadow_eggs_lock);
	list_add(entry, &memory->shadow_eggs);
	atomic_inc(&memory->pending_migration);
	spin_unlock(&memory->shadow_eggs_lock);

	complete(&memory->spawn_egg);
}


int shadow_spawner(void *_args)
{
	memory_t *memory = (memory_t *)_args;
	struct work_struct *work;

	printk(KERN_INFO"%s: started %d %p\n", __func__, current->pid, _args);
	current->main = false;

	while (true) {
		clone_request_t *req;
		wait_for_completion(&memory->spawn_egg);

		if (memory->helper_stop) {
			break;
		}

		spin_lock(&memory->shadow_eggs_lock);
		if (!list_empty(&memory->shadow_eggs)) {
			work = list_first_entry(
					&memory->shadow_eggs, struct work_struct, entry);
			list_del(&work->entry);
		} else {
			work = NULL;
		}
		spin_unlock(&memory->shadow_eggs_lock);

		req = ((clone_work_t *)work)->request;
		while (spawn_shadow_thread(req) == -EAGAIN) {
			schedule();
		}
		pcn_kmsg_free_msg(req);
		kfree(work);
	}

	return 0;
}


static struct task_struct *create_shadow_spawner(void)
{
	const unsigned long flags = CLONE_THREAD | CLONE_SIGHAND | SIGCHLD;
	memory_t *memory = current->memory;
	pid_t pid;

	BUG_ON(memory->shadow_spawner);

	pid = kernel_thread(shadow_spawner, memory, flags);
	BUG_ON(pid < 0);
	memory->shadow_spawner = find_task_by_vpid(pid);
	get_task_struct(memory->shadow_spawner);

	return NULL;
}


static int __construct_helper_mm(clone_request_t *req)
{
	struct mm_struct *mm;
	struct file *f;
	struct cred * new;

	/* sanghoon: I think there is no point to deal with user-level stuff for
	 * kernel thread...
	*/
	spin_lock_irq(&current->sighand->siglock);
	flush_signal_handlers(current, 1);
	spin_unlock_irq(&current->sighand->siglock);

	set_cpus_allowed_ptr(current, cpu_all_mask);
	set_user_nice(current, 0);
	new = prepare_kernel_cred(current);
	commit_creds(new);

	mm = mm_alloc();
	BUG_ON(!mm);

	arch_pick_mmap_layout(mm);
	// atomic_inc to prevent mm from being released during exec_mmap
	atomic_inc(&mm->mm_users);
	exec_mmap(mm);
	atomic_dec(&mm->mm_users);
	set_fs(USER_DS);
	flush_thread();
	flush_signal_handlers(current, 0);

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


/*
 * inform all kernel that a new distributed process is present here
 * the list does not include the current processor group descirptor (TODO)
 */
/*
int inform_process_migration()
{
	new_kernel_t *new_kernel_msg;
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

struct helper_params {
	clone_request_t *req;
	memory_t *memory;
	struct completion complete;
};

/**
 * Remote side kernel thread
 */
static int create_helper_thread(void *_data)
{
	struct helper_params *params = (struct helper_params *)_data;
	clone_request_t *req = params->req;
	memory_t *memory = params->memory;

	might_sleep();
	PSPRINTK("%s: task=0x%p, pid=%d\n", __func__, current, current->pid);
	PSPRINTK("%s: exe_path=%s\n", __func__, req->exe_path);
	PSPRINTK("%s: cpu=%d, id=%d, prev_pid=%d, origin_pid=%d\n", __func__,
			req->tgroup_home_cpu, req->tgroup_home_id,
			req->prev_pid, req->origin_pid);

	current->tgroup_distributed = 1;
	current->tgroup_home_cpu = req->tgroup_home_cpu;
	current->tgroup_home_id = req->tgroup_home_id;
	current->main = 1;

	if (__construct_helper_mm(req) != 0) {
		BUG();
		return -EINVAL;
	}
	atomic_inc(&current->mm->mm_users);
	memory->mm = current->mm;
	current->memory = memory;

	create_shadow_spawner();

	complete(&params->complete);
	kfree(params);
	// __inform_process_migration()

	return helper_thread_main();
}

///////////////////////////////////////////////////////////////////////////////
// handle clone
///////////////////////////////////////////////////////////////////////////////

static void clone_remote_thread(struct work_struct *_work)
{
	/* Processing clone requests are serialized by clone_wq.
	 * Thus we avoid introducing a new lock memory_entry
	 */
	clone_work_t *work = (clone_work_t *)_work;
	clone_request_t *req = (clone_request_t *)work->request;
	memory_t *memory;

	memory = find_memory_entry_in(req->tgroup_home_cpu, req->tgroup_home_id);
	if (!memory) {
		pid_t pid;
		struct task_struct *tsk;
		struct helper_params *params = kmalloc(sizeof(*params), GFP_KERNEL);
		// Allocate memory instance
		memory = __alloc_memory_struct(
				req->tgroup_home_cpu, req->tgroup_home_id);
		add_memory_entry_in(memory, req->tgroup_home_cpu);

		params->memory = memory;
		params->req = req;
		init_completion(&params->complete);

		// Start the helper thread
		pid = kernel_thread(create_helper_thread, params, 0);
		BUG_ON(pid < 0);
		tsk = find_task_by_vpid(pid);
		get_task_struct(tsk);
		memory->helper = tsk;

		wait_for_completion(&params->complete);
	}

	PSPRINTK("%s: memory at 0x%p\n", __func__, memory);
	// Kick the spawner
	__kick_shadow_spawner(memory, work);
	return;
}


static int handle_clone_request(struct pcn_kmsg_message *msg)
{
	clone_request_t *req = (clone_request_t *)msg;
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

	/* Register handlers */
	pcn_kmsg_register_callback(PCN_KMSG_TYPE_PROC_SRV_NEW_KERNEL,
				   handle_new_kernel);
	pcn_kmsg_register_callback(PCN_KMSG_TYPE_PROC_SRV_NEW_KERNEL_RESPONSE,
				   handle_new_kernel_answer);

	pcn_kmsg_register_callback(PCN_KMSG_TYPE_PROC_SRV_MIGRATE,
				   handle_clone_request);
	pcn_kmsg_register_callback(PCN_KMSG_TYPE_PROC_SRV_BACK_MIGRATE,
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
	return 0;
}
