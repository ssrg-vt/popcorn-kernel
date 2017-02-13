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
#include <linux/cpu_namespace.h>

#include <asm/kdebug.h>
#include <asm/process_server.h>
#include <popcorn/bundle.h>
#include <popcorn/cpuinfo.h>
#include <popcorn/debug.h>
#include <popcorn/process_server.h>

#include "types.h"
#include "vma_server.h"
#include "stat.h"

#undef CHECK_FOR_DUPLICATES

struct list_head remote_contexts[2];
spinlock_t remote_contexts_lock[2];

struct remote_context *get_task_remote(struct task_struct *tsk)
{
	struct remote_context *rc = tsk->mm->remote;
	atomic_inc(&rc->count);

	return rc;
}

bool put_task_remote(struct task_struct *tsk)
{
	struct remote_context *rc = tsk->mm->remote;
	bool last = atomic_dec_and_test(&rc->count);
	if (last) {
		tsk->mm->remote = NULL;
		list_del(&rc->list);
		rc->vma_worker_stop = true;
		wake_up_process(rc->vma_worker);
		complete(&rc->spawn_egg);
		smp_mb();
	}
	return last;
}

enum {
	INDEX_OUTBOUND = 0,
	INDEX_INBOUND = 1,
};


/* Hold the correnponding remote_contexts_lock */
static struct remote_context *__lookup_remote_contexts_out(int tgid)
{
	struct remote_context *rc;

	list_for_each_entry(rc, remote_contexts + INDEX_OUTBOUND, list) {
		if (rc->tgid == tgid) {
			return rc;
		}
	}
	return NULL;
}

static struct remote_context *__lookup_remote_contexts_in(int nid, int tgid)
{
	struct remote_context *rc;

	list_for_each_entry(rc, remote_contexts + INDEX_INBOUND, list) {
		if (rc->remote_tgids[nid] == tgid) {
			return rc;
		}
	}
	return NULL;
}

#define __lock_remote_contexts(index) \
	spin_lock(remote_contexts_lock + index)
#define __lock_remote_contexts_in(nid) \
	__lock_remote_contexts(INDEX_INBOUND)
#define __lock_remote_contexts_out(nid) \
	__lock_remote_contexts(INDEX_OUTBOUND)

#define __unlock_remote_contexts(index) \
	spin_unlock(remote_contexts_lock + index)
#define __unlock_remote_contexts_in(nid) \
	__unlock_remote_contexts(INDEX_INBOUND)
#define __unlock_remote_contexts_out(nid) \
	__unlock_remote_contexts(INDEX_OUTBOUND)

#define __remote_contexts_in() remote_contexts[INDEX_INBOUND]
#define __remote_contexts_out() remote_contexts[INDEX_OUTBOUND]


static struct remote_context *__alloc_remote_context(int nid, int tgid)
{
	struct remote_context *rc = kzalloc(sizeof(*rc), GFP_KERNEL);
	BUG_ON(!rc);

	INIT_LIST_HEAD(&rc->list);

	rc->tgid = tgid;

	atomic_set(&rc->count, 0);

	rc->shadow_spawner = NULL;
	INIT_LIST_HEAD(&rc->shadow_eggs);
	spin_lock_init(&rc->shadow_eggs_lock);
	init_completion(&rc->spawn_egg);

	memset(&rc->remote_tgids, 0x00, sizeof(rc->remote_tgids));

	INIT_LIST_HEAD(&rc->pages);
	spin_lock_init(&rc->pages_lock);

	INIT_LIST_HEAD(&rc->vmas);
	spin_lock_init(&rc->vmas_lock);

	printk(KERN_INFO"%s: at 0x%p for %d / %d\n", __func__, rc, nid, tgid);

	return rc;
}

void exit_remote_context(struct remote_context *rc)
{
	if (!rc) return;

	printk("%s: %p\n", __func__, rc);
}



///////////////////////////////////////////////////////////////////////////////
// Specialized functions (TODO need massive refactoring)
///////////////////////////////////////////////////////////////////////////////
/*
 * Functions to manipulate count list
 * head: _count_head
 * lock: _count_head_lock
 */
LIST_HEAD(_count_head);
DEFINE_SPINLOCK(_count_head_lock);

typedef struct count_answers {
	struct list_head list;

	int tgroup_home_cpu;
	int tgroup_home_id;
	int responses;
	int expected_responses;
	int count;
	spinlock_t lock;
	struct task_struct * waiting;
} count_answers_t;

static inline void add_count_entry(count_answers_t *entry)
{
	unsigned long flags;

	BUG_ON(!entry);

	spin_lock_irqsave(&_count_head_lock, flags);
	list_add_tail(&entry->list, &_count_head);
	spin_unlock_irqrestore(&_count_head_lock, flags);
}

static inline count_answers_t* find_count_entry(int cpu, int id)
{
	count_answers_t* e = NULL;
	count_answers_t* found = NULL;
	unsigned long flags;

	spin_lock_irqsave(&_count_head_lock, flags);
	list_for_each_entry(e, &_count_head, list) {
		if (e->tgroup_home_cpu == cpu && e->tgroup_home_id == id) {
#ifdef CHECK_FOR_DUPLICATES
			WARN(found,
				printk(KERN_ERR"%s: duplicated %s %s (cpu %d id %d)\n",
							__func__,
							found->waiting ? found->waiting->comm : "?",
							e->waiting ? e->waiting->comm : "?",
							cpu, id);
			}
			found = e;
#else
			found = e;
			break;
#endif
		}
	}
	spin_unlock_irqrestore(&_count_head_lock, flags);

	return found;
}

static inline void remove_count_entry(count_answers_t* entry)
{
	unsigned long flags;

	BUG_ON(!entry);

	spin_lock_irqsave(&_count_head_lock, flags);
	list_del_init(&entry->list);
	spin_unlock_irqrestore(&_count_head_lock, flags);
}

///////////////////////////////////////////////////////////////////////////////
// Marina's data stores (linked lists)
///////////////////////////////////////////////////////////////////////////////

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
				printk(KERN_ERR"%s: duplicated %s %s (cpu %d id %d)\n",
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


///////////////////////////////////////////////////////////////////////////////
// Common functions
///////////////////////////////////////////////////////////////////////////////

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


///////////////////////////////////////////////////////////////////////////////
// Working queues (servers)
///////////////////////////////////////////////////////////////////////////////

static struct workqueue_struct *clone_wq;
static struct workqueue_struct *exit_wq;
static struct workqueue_struct *exit_group_wq;
static struct workqueue_struct *new_kernel_wq;

static int count_remote_thread_members(
		int tgroup_home_cpu, int tgroup_home_id, memory_t *memory)
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


///////////////////////////////////////////////////////////////////////////////
// handling a new incoming migration request?
///////////////////////////////////////////////////////////////////////////////
struct new_kernel_work_answer {
	struct work_struct work;
	new_kernel_response_t* answer;
	memory_t* memory;
};

static void process_new_kernel_response(struct work_struct *_work)
{
	int i;
	struct new_kernel_work_answer *work = (struct new_kernel_work_answer *)_work;
	new_kernel_response_t *answer = work->answer;
	memory_t *memory = work->memory;

	if (answer->header.from_cpu == answer->tgroup_home_cpu) {
		down_write(&memory->mm->mmap_sem);
		// printk("%s answer->vma_operation_index %d NR_CPU %d\n", __func__,
		// answer->vma_operation_index, MAX_KERNEL_IDS);
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

static int handle_new_kernel_response(struct pcn_kmsg_message *inc_msg)
{
	new_kernel_response_t *answer = (new_kernel_response_t *)inc_msg;
	memory_t *memory = find_memory_entry(answer->tgroup_home_cpu,
					    answer->tgroup_home_id);
	struct new_kernel_work_answer *work;

	PSNEWTHREADPRINTK("received new kernel answer\n");
	//printk("%s: %d\n", __func__, answer->vma_operation_index);
	if (memory == NULL) {
		printk("%s: ERROR: received an answer for new kernel "
				"but memory_t not present cpu %d id %d\n", __func__,
				answer->tgroup_home_cpu, answer->tgroup_home_id);
		pcn_kmsg_free_msg(inc_msg);
		return 1;
	}
	
	work = kmalloc(sizeof(*work), GFP_ATOMIC);
	BUG_ON(!work);
	work->answer = answer;
	work->memory = memory;
	INIT_WORK((struct work_struct *)work, process_new_kernel_response);
	queue_work(new_kernel_wq, (struct work_struct *)work);

	return 1;
}

static void process_new_kernel(struct work_struct *_work)
{
	struct pcn_kmsg_work *work = (struct pcn_kmsg_work *)_work;
	new_kernel_t *req = work->msg;
	memory_t *memory;
	new_kernel_response_t *answer = kmalloc(sizeof(*answer), GFP_ATOMIC);
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

		if (my_nid() == req->tgroup_home_cpu) {
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


/* return type:
 * 0 normal;
 * 1 flush pending operation
 * */
int exit_distributed_process(memory_t *memory, int flush)
{
	struct task_struct *thread;
	unsigned long flags;
	int is_last_thread_in_local_group = true;
	int count = 0, i, status;

	lock_task_sighand(current, &flags);
	for_each_thread(current, thread) {
		if (thread->is_vma_worker == 0 && thread->distributed_exit == EXIT_ALIVE) {
			is_last_thread_in_local_group = false;
			goto found;
		}
	};

found:
	status = current->distributed_exit;
	current->distributed_exit = EXIT_ALIVE;
	unlock_task_sighand(current, &flags);

	if (memory->alive == false
			&& !is_last_thread_in_local_group
			&& atomic_read(&memory->pending_migration) == 0) {
		printk("%s: ERROR: memory->alive is 0 but there are alive threads "
				"(id %d, cpu %d)\n", __func__,
				current->origin_pid, current->origin_nid);
		return 0;
	}

	if (memory->alive == false
			&& atomic_read(&memory->pending_migration) == 0) {

		if (status == EXIT_THREAD) {
			printk("%s: ERROR: alive is 0 but status is exit thread "
				"(id %d, cpu %d)\n", __func__,
				current->origin_pid, current->origin_nid);
			return flush;
		}

		if (status == EXIT_PROCESS) {
			if (flush == 0) {
				// Required to flush the list of pending operation before die
				memory->arrived_op = 0;
				vma_server_enqueue_vma_op(memory, 0, 1);
				return 1;
			}
		}

		if (flush == 1 && memory->arrived_op == 0) {
			if (status == EXIT_FLUSHING)
				printk("%s: ERROR: status exit flush but arrived op is 0 "
						"(id %d, cpu %d)\n", __func__,
						current->origin_pid, current->origin_nid);
			return 1;
		}

		if (atomic_read(&memory->pending_migration) != 0)
			printk(KERN_ALERT"%s: ERROR pending migration when cleaning memory "
					"(id %d, cpu %d)\n", __func__,
					current->origin_pid, current->origin_nid);

		/*
		 * Empty unused shadow thread pool

		shadow_thread_t *my_shadow = NULL;
		my_shadow = (shadow_thread_t *)pop_data((data_header_t **)
					&(my_thread_pool->threads), &(my_thread_pool->spinlock));

		while (my_shadow){
			my_shadow->thread->distributed_exit = EXIT_THREAD;
			wake_up_process(my_shadow->thread);
			kfree(my_shadow);
			my_shadow = (shadow_thread_t *)pop_data((data_header_t **)
					&(my_thread_pool->threads), &(my_thread_pool->spinlock));
		}
		*/
		remove_memory_entry(memory);
		mmput(memory->mm);
		kfree(memory);

#if STATISTICS
		print_popcorn_stat();
#endif
		return 0;
	}


	/* If I am the last thread of my process in this kernel:
	 * - or I am the last thread of the process on all the system
	 *   => send a group exit to all kernels and erase the mapping saved
	 * - or there are other alive threads in the system
	 *   => do not erase the saved mapping
	 */
	if (is_last_thread_in_local_group) {
		PSPRINTK("%s: This is the last thread of process (id %d, cpu %d) "
				"in the kernel!\n", __func__,
				current->origin_pid, current->origin_nid);
		//memory->alive = 0;
		count = count_remote_thread_members(
				current->origin_nid, current->origin_pid, memory);
		/* Ok this is complicated.
		 * If count is zero
		 *	=> all the threads of my process went through this exit function
		 *  (all task->distributed_exit == 1 or there are no more tasks of this
		 *  process around).
		 * Dying tasks that did not see count == 0 saved a copy of the mapping.
		 * Someone should notice their kernels that now they can erase it.
		 * I can be the one, however more threads can be concurrently in this
		 * exit function on different kernels =>
		 * each one of them can see the count == 0
		 *	=> more than one "erase mapping message" can be sent.
		 * If count == 0 I check if I already receive a "erase mapping message"
		 * and avoid to send another one.
		 * This check does not guarantee that more than one "erase mapping
		 * message" cannot be sent (in some executions it is inevitable)
		 *	=> just be sure to not call more than one mmput one the same
		 *	mapping!!!
		 */
		if (count == 0) {
			memory->alive = false;
			if (status != EXIT_PROCESS) {
				_remote_cpu_info_list_t *r;
				thread_group_exited_notification_t *exit_notification =
					kmalloc(sizeof(*exit_notification), GFP_ATOMIC);

				PSPRINTK("%s: This is the last thread of process "
						"(id %d, cpu %d) in the system, "
						"sending an erase mapping message!\n", __func__,
						current->origin_pid, current->origin_nid);
				exit_notification->header.type =
						PCN_KMSG_TYPE_PROC_SRV_THREAD_GROUP_EXITED_NOTIFICATION;
				exit_notification->header.prio = PCN_KMSG_PRIO_NORMAL;
				exit_notification->tgroup_home_cpu = current->origin_nid;
				exit_notification->tgroup_home_id = current->origin_pid;

				// TODO: the list does not include the current processor group
				// descirptor
				list_for_each_entry(r, &rlist_head, cpu_list_member) {
					i = r->_data._processor;
					pcn_kmsg_send_long(
							i, exit_notification, sizeof(exit_notification));

				}
				kfree(exit_notification);
			}

			if (flush == 0) {
				// Requred to flush the list of pending operation before die
				memory->arrived_op = 0;
				vma_server_enqueue_vma_op(memory, 0, 1);
			} else {
				printk("%s: ERROR: flush is 1 during first exit "
						"(alive set to 0 now) (id %d, cpu %d)\n", __func__,
						current->origin_pid, current->origin_nid);
			}
			return 1;
		}
		/*
		 * case i am the last thread but count is not zero
		 * check if there are concurrent migration to be sure if I can
		 * put memory->alive = 0;
		 */
		if (atomic_read(&(memory->pending_migration)) == 0)
			memory->alive = false;
	}

	if ((!is_last_thread_in_local_group || count != 0)
			&& status == EXIT_PROCESS) {
		printk("%s: ERROR: received an exit process "
				"but is_last_thread_in_local_group id %d and count is %d\n ",
			   __func__, is_last_thread_in_local_group, count);
	}
	return 0;
}


///////////////////////////////////////////////////////////////////////////////
// exit group notification
///////////////////////////////////////////////////////////////////////////////
static void process_thread_group_exited_notification(struct work_struct *_work)
{
	struct pcn_kmsg_work *work = (struct pcn_kmsg_work *)_work;
	thread_group_exited_notification_t *msg = work->msg;
	unsigned long flags;

	memory_t *memory = find_memory_entry(msg->tgroup_home_cpu,
					      msg->tgroup_home_id);
	printk("%s: %p\n", __func__, memory);
	if (!memory)
		goto out_free;

	while (memory->helper == NULL)
		schedule();

	lock_task_sighand(memory->helper, &flags);
	memory->helper->distributed_exit = EXIT_PROCESS;
	unlock_task_sighand(memory->helper, &flags);

out_free:
	pcn_kmsg_free_msg(msg);
	kfree(work);
}


int process_server_task_exit(struct task_struct *tsk)
{
	struct remote_context *rc;

	if (!process_is_distributed(tsk)) return -ESRCH;

	rc = tsk->mm->remote;

	printk("exit: Stopping 0x%p %d %d\n", tsk, tsk->pid, tsk->tgid);
	printk("exit: 0x%p %d / %d, %d / %d, %d\n",
			rc, atomic_read(&rc->count),
			tsk->is_vma_worker, tsk->is_shadow,
			tsk->exit_code, tsk->group_exit);
	//__show_regs(regs, 1);

	/* I am helper */
	if (tsk->is_vma_worker == 1) {
		return 0;
	}

	if (tsk->is_shadow) {
		// I am a remote process
		task_exit_t *req = kzalloc(sizeof(*req), GFP_KERNEL);
		BUG_ON(!req);

		req->header.type = PCN_KMSG_TYPE_PROC_SRV_TASK_EXIT;
		req->header.prio = PCN_KMSG_PRIO_NORMAL;
		req->origin_pid = tsk->origin_pid;
		req->remote_pid = tsk->pid;
		req->exit_code = tsk->exit_code;
		req->group_exit = tsk->group_exit;

		tsk->migration_pc = task_pt_regs(tsk)->ip;
		save_thread_info(tsk, task_pt_regs(tsk), &req->arch, NULL);

		pcn_kmsg_send_long(tsk->origin_nid, req, sizeof(*req));
		kfree(req);
	} else {
		// I am a local process
		if (tsk->remote_nid != -1 || tsk->remote_pid != -1) {
			WARN_ON("Don't forget to terminate the remote task");
		}
	}

	if (put_task_remote(tsk)) {
		if (tsk->is_shadow) {
			// TODO transfer all owned pages to the origin location
		}
	}

	return 0;
}


/**
 * Notify of the fact that either a delegate or placeholder has died locally.
 * In this case, the remote cpu housing its counterpart must be notified, so
 * that it can kill that counterpart.
 */
static void process_exit_task(struct work_struct *_work)
{
	struct pcn_kmsg_work *work = (struct pcn_kmsg_work *)_work;
	task_exit_t *req = work->msg;

	struct task_struct *tsk;

	rcu_read_lock();
	tsk = find_task_by_vpid(req->origin_pid);
	rcu_read_unlock();

	if (tsk && tsk->remote_pid == req->remote_pid) {
		printk(KERN_INFO"%s: exit %d with %ld, %d\n", __func__,
				tsk->pid, req->exit_code, req->group_exit);

		restore_thread_info(tsk, &req->arch);

		tsk->distributed_exit_code = req->exit_code;
		tsk->group_exit = req->group_exit;

		put_task_remote(tsk);
		wake_up_process(tsk);
	} else {
		printk(KERN_INFO"%s: task not found (pid %d)\n",
				__func__, req->origin_pid);
	}

	pcn_kmsg_free_msg(req);
	kfree(work);
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

static void process_remote_thread_count_request(struct work_struct *_work)
{
	struct pcn_kmsg_work *work = (struct pcn_kmsg_work *)_work;
	remote_thread_count_request_t *request = work->msg;
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
			if (t->distributed_exit == EXIT_ALIVE && t->is_vma_worker != 1) {
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



///////////////////////////////////////////////////////////////////////////////
// handling back migration
///////////////////////////////////////////////////////////////////////////////
static int handle_back_migration(struct pcn_kmsg_message *inc_msg)
{
	back_migration_request_t *req = (back_migration_request_t *)inc_msg;
	struct task_struct *tsk;

#if MIGRATION_PROFILE
	migration_start = ktime_get();
#endif

	PSPRINTK(" IN %s: %d %d\n", __func__,
			req->origin_pid, req->remote_pid);

	rcu_read_lock();
	tsk = find_task_by_vpid(req->origin_pid);
	rcu_read_unlock();

	if (!tsk) {
		printk("%s: no origin taks %d for remote_pid %d\n",
				__func__, req->origin_pid, req->remote_pid);
		goto out_free;
	}

	get_task_struct(tsk);

	if (tsk->remote_pid == req->remote_pid) {
		/* Welcome home */
		restore_thread_info(tsk, &req->arch);
		initialize_thread_retval(tsk, 0);

		tsk->remote_nid = -1;
		tsk->remote_pid = -1;
		tsk->personality = req->personality;
		smp_wmb();

		put_task_remote(tsk);
		wake_up_process(tsk);

#if MIGRATION_PROFILE
		migration_end = ktime_get();
  		printk(KERN_ERR"Time for x86->arm back migration - %s side: %ld ns\n",
				MY_ARCH, GET_MIGRATION_TIME);
#endif
	} else {
		printk("%s: %d has remote %d != %d", __func__,
				tsk->pid, tsk->remote_pid, req->remote_pid);
	}
	put_task_struct(tsk);

out_free:
	pcn_kmsg_free_msg(req);
	return 0;
}


/*
 * Send a message to <dst_nid> for migrating back a task <task>.
 * This is a back migration
 *  => <task> must already been migrated to <dst_nid>.
 * It returns -1 in error case.
 */
static int do_back_migration(struct task_struct *tsk, int dst_nid, void __user *uregs)
{
	struct pt_regs *regs = task_pt_regs(tsk);
	back_migration_request_t *req;
	int ret;

	might_sleep();

	req = kmalloc(sizeof(*req), GFP_KERNEL);
	BUG_ON(req);

	PSPRINTK("%s\n", __func__);

	//printk("%s entered dst{%d}\n", __func__, dst_cpu);
	req->header.type = PCN_KMSG_TYPE_PROC_SRV_BACK_MIGRATE;
	req->header.prio = PCN_KMSG_PRIO_NORMAL;

	req->origin_pid = tsk->origin_pid;
	req->remote_nid = my_nid();
	req->remote_pid = tsk->pid;

	req->personality = tsk->personality;
	req->remote_blocked = tsk->blocked;
	req->remote_real_blocked = tsk->real_blocked;
	req->remote_saved_sigmask = tsk->saved_sigmask;
	req->remote_pending = tsk->pending;
	req->sas_ss_sp = tsk->sas_ss_sp;
	req->sas_ss_size = tsk->sas_ss_size;
	memcpy(req->action, tsk->sighand->action, sizeof(req->action));

	save_thread_info(tsk, regs, &req->arch, uregs);

	ret = pcn_kmsg_send_long(dst_nid, req, sizeof(*req));

#if MIGRATION_PROFILE
	migration_end = ktime_get();
	printk(KERN_ERR"Time for x86->arm back migration - x86 side: %ld ns\n",
			GET_MIGRATION_TIME);
#endif
	kfree(req);
	// Exit this thread ok?
	//
	return ret;
}


/**
 * Main loop for in-kernel request handler
 */
#define GET_UNMAP_IF_HOME(task, memory) { \
if (task->tgroup_home_cpu != my_nid()) { \
	WARN(memory->mm->distribute_unmap == 0, \
		"GET_UNMAP_IF_HOME: value was already 0, check who is the older.\n"); \
	memory->mm->distribute_unmap = 0; \
	} \
}
#define PUT_UNMAP_IF_HOME(task, memory) { \
if (task->tgroup_home_cpu != my_nid()) {\
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


///////////////////////////////////////////////////////////////////////////////
// pairing request
///////////////////////////////////////////////////////////////////////////////
static int handle_process_pairing_request(struct pcn_kmsg_message *msg)
{
	create_process_pairing_t *req = (create_process_pairing_t *)msg;
	unsigned int nid_from = req->header.from_cpu;
	struct task_struct *tsk;

	rcu_read_lock();
	tsk = find_task_by_vpid(req->your_pid);
	rcu_read_unlock();
	if (tsk == NULL) {
		pcn_kmsg_free_msg(req);
		return -ESRCH;
	}
	BUG_ON(tsk->is_shadow);

	tsk->remote_nid = nid_from;
	tsk->remote_pid = req->my_pid;

	PSPRINTK("Pairing local:  %d --> %d at %d\n",
			tsk->pid, nid_from, req->my_pid);

	pcn_kmsg_free_msg(req);
	return 0;
}

static int __pair_remote_process(struct task_struct *tsk)
{
	create_process_pairing_t *req;
	int ret;

	req = kmalloc(sizeof(*req), GFP_KERNEL);
	BUG_ON(!req);

	// Notify remote cpu of pairing between current task and remote
	// representative task.
	req->header.type = PCN_KMSG_TYPE_PROC_SRV_CREATE_PROCESS_PAIRING;
	req->header.prio = PCN_KMSG_PRIO_NORMAL;
	req->my_pid = current->pid;
	req->your_pid = current->origin_pid;

	PSPRINTK("Pairing remote: %d --> %d at %d\n",
			req->my_pid, req->your_pid, current->origin_nid);

	ret = pcn_kmsg_send_long(current->origin_nid, req, sizeof(*req));
	kfree(req);

	return ret;
}


///////////////////////////////////////////////////////////////////////////////
// Remote-side thread
///////////////////////////////////////////////////////////////////////////////

struct shadow_params {
	clone_request_t *req;
};

int shadow_main(void *_args)
{
	struct shadow_params *params = _args;
	clone_request_t *req = params->req;

    PSPRINTK("%s: start %d at 0x%p\n", __func__, current->pid, current);

	__rename_task_comm(current, req->exe_path);

	current->flags &= ~(PF_KTHREAD | PF_RANDOMIZE);	// Convert to user
	current->origin_nid = req->origin_nid;
	current->origin_pid = req->origin_pid;
	current->personality = req->personality;
	current->is_shadow = true;
	current->distributed_exit = EXIT_ALIVE;

	// set thread info
	restore_thread_info(current, &req->arch);
	initialize_thread_retval(current, 0);

	/*
	current->origin_pid = req->origin_pid;
	sigorsets(&current->blocked,&current->blocked, &req->remote_blocked) ;
	sigorsets(&current->real_blocked, &current->real_blocked, &req->remote_real_blocked);
	sigorsets(&current->saved_sigmask, &current->saved_sigmask, &req->remote_saved_sigmask);
	current->pending = req->remote_pending;
	current->sas_ss_sp = req->sas_ss_sp;
	current->sas_ss_size = req->sas_ss_size;
	memcpy(current->sighand->action, req->action, sizeof(req->action));
	*/

#if MIGRATION_PROFILE
	migration_end = ktime_get();
	printk(KERN_ERR"Time for arm->x86 migration - x86 side: %ld ns\n",
			GET_MIGRATION_TIME);
#endif

	printk("####### MIGRATED - %d at %d --> %d at %d\n",
			current->origin_pid, current->origin_nid, current->pid, my_nid());
	printk("%s: pc %lx ip %lx\n", __func__, 
			(&req->arch)->migration_pc, current_pt_regs()->ip);
	printk("%s: sp %lx bp %lx\n", __func__, 
			current_pt_regs()->sp, current_pt_regs()->bp);


	// Notify of PID/PID pairing.
	__pair_remote_process(current);

	kfree(params);
	pcn_kmsg_free_msg(req);

	// TODO: Handle return values
	update_thread_info(current);

	return 0;	/* Returning from here will start the user thread run */
}


static void __kick_shadow_spawner(struct remote_context *rc, struct pcn_kmsg_work *work)
{
	/* Utilize the list_head in work_struct */
	struct list_head *entry = &((struct work_struct *)work)->entry;

	atomic_inc(&rc->count);

	INIT_LIST_HEAD(entry);
	spin_lock(&rc->shadow_eggs_lock);
	list_add(entry, &rc->shadow_eggs);
	spin_unlock(&rc->shadow_eggs_lock);

	complete(&rc->spawn_egg);
}


int shadow_spawner(void *_args)
{
	struct remote_context *rc = _args;
	struct work_struct *work;
	struct shadow_params *params;

	PSPRINTK(KERN_INFO"%s: started %d\n", __func__, current->pid);
	current->is_vma_worker = true;

	while (!rc->vma_worker_stop) {
		clone_request_t *req;
		wait_for_completion(&rc->spawn_egg);

		spin_lock(&rc->shadow_eggs_lock);
		if (!list_empty(&rc->shadow_eggs)) {
			work = list_first_entry(
					&rc->shadow_eggs, struct work_struct, entry);
			list_del(&work->entry);
		} else {
			work = NULL;
		}
		spin_unlock(&rc->shadow_eggs_lock);

		if (!work) continue;

		params = kmalloc(sizeof(*params), GFP_KERNEL);
		req = ((struct pcn_kmsg_work *)work)->msg;
		do {
			const unsigned long flags = CLONE_THREAD | CLONE_SIGHAND | SIGCHLD;

			params->req = req;

			if (kernel_thread(shadow_main, params, flags) >= 0) {
				break;
			}
			schedule();
		} while (true);
		kfree(work);
	}

	PSPRINTK(KERN_INFO"%s: exited %d\n", __func__, current->pid);
	return 0;
}


static int __construct_vma_worker_mm(clone_request_t *req, struct remote_context *rc)
{
	struct mm_struct *mm;
	struct file *f;
	struct cred *new;

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
	/* atomic_inc to prevent mm from being released during exec_mmap */
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

	atomic_inc(&mm->mm_users);
	rc->mm = mm;
	mm->remote = rc;

	return 0;
}


struct vma_worker_params {
	struct pcn_kmsg_work *work;
	struct remote_context *rc;
};

static int vma_worker_at_remote(void *_data)
{
	struct vma_worker_params *params = (struct vma_worker_params *)_data;
	struct pcn_kmsg_work *work = params->work;
	clone_request_t *req = work->msg;
	struct remote_context *rc = params->rc;
	pid_t pid;

	might_sleep();
	PSPRINTK("%s: task=0x%p pid=%d\n", __func__, current, current->pid);
	PSPRINTK("%s: exe_path=%s\n", __func__, req->exe_path);
	PSPRINTK("%s: origin_nid=%d origin_tgid=%d origin_pid=%d\n",
			__func__, req->origin_nid, req->origin_tgid, req->origin_pid);

	if (__construct_vma_worker_mm(req, rc) != 0) {
		BUG();
		return -EINVAL;
	}

	rc->tgid = current->tgid;
	rc->remote_tgids[my_nid()] = rc->tgid;
	rc->remote_tgids[req->origin_nid] = req->origin_tgid;
	get_task_struct(current); /* for rc->vma_worker */
	rc->vma_worker = current;

	current->is_vma_worker = 1;
	current->origin_pid = req->origin_pid;

	/* Create the shadow spawner */
	pid = kernel_thread(shadow_spawner, rc,
			CLONE_THREAD | CLONE_SIGHAND | SIGCHLD);
	BUG_ON(pid < 0);
	rcu_read_lock();
	rc->shadow_spawner = find_task_by_vpid(pid);
	rcu_read_unlock();

	barrier();
	list_add(&rc->list, &__remote_contexts_in());
	__unlock_remote_contexts_in(nid_from);

	kfree(params);
	vma_worker_main(rc, "remote");
	do_exit(0);
}


static void clone_remote_thread(struct work_struct *_work)
{
	struct pcn_kmsg_work *work = (struct pcn_kmsg_work *)_work;
	clone_request_t *req = work->msg;
	int nid_from = req->origin_nid;
	int tgid_from = req->origin_tgid;
	struct remote_context *rc;
	struct remote_context *rc_new =
			__alloc_remote_context(nid_from, tgid_from);
	struct vma_worker_params *params = kmalloc(sizeof(*params), GFP_KERNEL);

	BUG_ON(!rc_new || !params);

	__lock_remote_contexts_in(nid_from);
	rc = __lookup_remote_contexts_in(nid_from, tgid_from);
	if (!rc) {
		pid_t pid;
		rc = rc_new;

		params->work = work;
		params->rc = rc;

		/*
		 * Start the helper thread. remote_contexts_lock is released upon
		 * thread creation completion
		 */
		pid = kernel_thread(vma_worker_at_remote, params, 0);
		BUG_ON(pid < 0);
	} else {
		kfree(rc_new);
		__unlock_remote_contexts_in(nid_from);
		printk("%s: found at %p\n", __func__, rc);
	}

	PSPRINTK("%s: remote_context at 0x%p\n", __func__, rc);
	// Kick the spawner
	__kick_shadow_spawner(rc, work);
	return;
}


static int handle_clone_request(struct pcn_kmsg_message *msg)
{
	clone_request_t *req = (clone_request_t *)msg;
	struct pcn_kmsg_work *work = kmalloc(sizeof(*work), GFP_ATOMIC);
	BUG_ON(!work);

#if MIGRATION_PROFILE
	migration_start = ktime_get();
#endif

	work->msg = req;
	INIT_WORK((struct work_struct *)work, clone_remote_thread);
	queue_work(clone_wq, (struct work_struct *)work);

	return 0;
}


/**
 * Send a message to <dst_cpu> for migrating a task <task>.
 * This function will ask the remote node to create a thread to host the task.
 * It returns -1 in error case.
 */
static int __request_clone_remote(int dst_nid, struct task_struct *tsk,
		struct pt_regs *regs, void __user *uregs)
{
	char *rpath, path[512];
	clone_request_t *req;
	int ret;

	might_sleep();

	req = kmalloc(sizeof(*req), GFP_KERNEL);
	BUG_ON(!req);

	/* Build request */
	req->header.type = PCN_KMSG_TYPE_PROC_SRV_MIGRATE;
	req->header.prio = PCN_KMSG_PRIO_NORMAL;

	// struct mm_struct --------------------------------------------------------
	rpath = d_path(&tsk->mm->exe_file->f_path, path, sizeof(path));
	if (IS_ERR(rpath)) {
		printk("%s: exe binary path is too long.\n", __func__);
		kfree(req);
		return -ESRCH;
	} else {
		strncpy(req->exe_path, rpath, sizeof(req->exe_path));
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

	// struct tsk_struct ------------------------------------------------------
	req->origin_nid = my_nid();
	req->origin_tgid = tsk->tgid;
	req->origin_pid = tsk->pid;

	req->personality = tsk->personality;

	req->remote_blocked = tsk->blocked;
	req->remote_real_blocked = tsk->real_blocked;
	req->remote_saved_sigmask = tsk->saved_sigmask;
	req->remote_pending = tsk->pending;
	req->sas_ss_sp = tsk->sas_ss_sp;
	req->sas_ss_size = tsk->sas_ss_size;
	memcpy(req->action, tsk->sighand->action, sizeof(req->action));

	save_thread_info(tsk, regs, &req->arch, uregs);

	ret = pcn_kmsg_send_long(dst_nid, req, sizeof(*req));
	kfree(req);

	return ret;
}


static int vma_worker_at_home(void *_arg)
{
	struct remote_context *rc = _arg;

	might_sleep();
	rc->vma_worker = current;
	current->is_vma_worker = true;

	vma_worker_main(rc, "local");
	do_exit(0);
}


/*
 * Create a kernel thread - required for process server to create a
 * kernel thread that share mm_struct with the user one
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

int do_migration(struct task_struct *tsk, int dst_nid, void __user *uregs)
{
	int ret;
	unsigned long flags;
	bool create_vma_worker = false;
	struct pt_regs *regs = task_pt_regs(tsk);
	struct remote_context *rc, *rc_new;

	might_sleep();

	/* Want to avoid allocate this structure in the spinlock-ed area */
	rc_new = __alloc_remote_context(my_nid(), tsk->tgid);

	__lock_remote_contexts_out(dst_nid);
	rc = __lookup_remote_contexts_out(tsk->tgid);
	if (!rc) {
		/*
		 * This process is becoming a distributed one if it was not already.
		 * The first migrating thread attaches the remote context to 
		 * mm->remote, which indicates this process is distributed, and
		 * forks the vma worker thread for this process.
		 */
		BUG_ON(process_is_distributed(tsk));
		rc = rc_new;

		/* I use siglock to coordinate the thread group. */
		lock_task_sighand(tsk, &flags);

		/*
		 * Setting mm->remote to remote_context indicates 
		 * this process is distributed
		 */ 
		tsk->mm->remote = rc;
		rc->mm = tsk->mm;
		rc->remote_tgids[my_nid()] = tsk->tgid;

		barrier();

		list_add(&rc->list, &__remote_contexts_out());

		create_vma_worker = true;
	} else {
		kfree(rc_new);
	}
	BUG_ON(!rc);
	rc = get_task_remote(tsk);
	tsk->is_shadow = false;
	unlock_task_sighand(tsk, &flags);
	__unlock_remote_contexts_out(dst_nid);

	if (create_vma_worker) {
		pid_t pid = kernel_thread_popcorn(vma_worker_at_home, rc);
		BUG_ON(pid < 0);
	}

	ret = __request_clone_remote(dst_nid, tsk, regs, uregs);

#if MIGRATION_PROFILE
	migration_end = ktime_get();
	printk(KERN_ERR"Time for x86->arm migration - x86 side: %ld ns\n",
			GET_MIGRATION_TIME);
#endif

	return ret;
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
int process_server_do_migration(struct task_struct *tsk, int dst_nid,
		void __user *uregs)
{
	int ret = 0;

	PSPRINTK(KERN_INFO"%s: pid=%d, tgid=%d\n", __func__, tsk->pid, tsk->tgid);

	if (tsk->origin_nid == dst_nid) {
		ret = do_back_migration(tsk, dst_nid, uregs);
	} else {
		ret = do_migration(tsk, dst_nid, uregs);
	}

	return ret;
}


DEFINE_KMSG_WQ_HANDLER(new_kernel, new_kernel_wq);
DEFINE_KMSG_WQ_HANDLER(exit_task, exit_wq);
DEFINE_KMSG_WQ_HANDLER(remote_thread_count_request, exit_wq);
DEFINE_KMSG_WQ_HANDLER(thread_group_exited_notification, exit_group_wq);

/**
 * Initialize the process server.
 */
int __init process_server_init(void)
{
	/**
	 * Create work queues so that we can do bottom side
	 * processing on data that was brought in by the
	 * communications module interrupt handlers.
	 */
	clone_wq = create_workqueue("clone_wq");
	exit_wq  = create_workqueue("exit_wq");
	exit_group_wq = create_workqueue("exit_group_wq");
	new_kernel_wq = create_workqueue("new_kernel_wq");

	INIT_LIST_HEAD(&remote_contexts[0]);
	INIT_LIST_HEAD(&remote_contexts[1]);

	spin_lock_init(&remote_contexts_lock[0]);
	spin_lock_init(&remote_contexts_lock[1]);

	/* Register handlers */
	REGISTER_KMSG_WQ_HANDLER(
			PCN_KMSG_TYPE_PROC_SRV_NEW_KERNEL, new_kernel);
	REGISTER_KMSG_WQ_HANDLER(
			PCN_KMSG_TYPE_PROC_SRV_THREAD_COUNT_REQUEST,
			remote_thread_count_request);
	REGISTER_KMSG_WQ_HANDLER(
			PCN_KMSG_TYPE_PROC_SRV_TASK_EXIT, exit_task);
	REGISTER_KMSG_WQ_HANDLER(
			PCN_KMSG_TYPE_PROC_SRV_THREAD_GROUP_EXITED_NOTIFICATION,
			thread_group_exited_notification);

	pcn_kmsg_register_callback(PCN_KMSG_TYPE_PROC_SRV_NEW_KERNEL_RESPONSE,
				   handle_new_kernel_response);

	pcn_kmsg_register_callback(PCN_KMSG_TYPE_PROC_SRV_MIGRATE,
				   handle_clone_request);
	pcn_kmsg_register_callback(PCN_KMSG_TYPE_PROC_SRV_BACK_MIGRATE,
				   handle_back_migration);
	pcn_kmsg_register_callback(PCN_KMSG_TYPE_PROC_SRV_CREATE_PROCESS_PAIRING,
				   handle_process_pairing_request);

	pcn_kmsg_register_callback(PCN_KMSG_TYPE_PROC_SRV_THREAD_COUNT_RESPONSE,
				   handle_remote_thread_count_response);
	return 0;
}
