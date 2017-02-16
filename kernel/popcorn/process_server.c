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

#include <linux/sched.h>
#include <linux/threads.h>
#include <linux/slab.h>
#include <linux/kthread.h>
#include <linux/sched.h>
#include <linux/ptrace.h>

#include <asm/mmu_context.h>
#include <asm/kdebug.h>

#include <popcorn/bundle.h>
#include <popcorn/cpuinfo.h>
#include <popcorn/debug.h>
#include <popcorn/process_server.h>

#include "types.h"
#include "vma_server.h"
#include "page_server.h"
#include "stat.h"

static struct workqueue_struct *clone_wq;
static struct workqueue_struct *exit_wq;


static struct list_head remote_contexts[2];
static spinlock_t remote_contexts_lock[2];

struct remote_context *get_task_remote(struct task_struct *tsk)
{
	struct remote_context *rc = tsk->mm->remote;
	atomic_inc(&rc->count);

	return rc;
}

bool put_task_remote(struct task_struct *tsk)
{
	return atomic_dec_and_test(&tsk->mm->remote->count);
}

enum {
	INDEX_OUTBOUND = 0,
	INDEX_INBOUND = 1,
};


/* Hold the correnponding remote_contexts_lock */
static struct remote_context *__lookup_remote_contexts_in(int nid, int tgid)
{
	struct remote_context *rc, *tmp;

	list_for_each_entry_safe(rc, tmp, remote_contexts + INDEX_INBOUND, list) {
		printk(" %p %d %d\n", rc, rc->remote_tgids[nid], tgid);
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


static struct remote_context *__alloc_remote_context(int nid, int tgid, bool remote)
{
	struct remote_context *rc = kzalloc(sizeof(*rc), GFP_KERNEL);
	BUG_ON(!rc);

	INIT_LIST_HEAD(&rc->list);

	rc->tgid = tgid;
	rc->for_remote = remote;

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
	barrier();

	printk(KERN_INFO"%s: at 0x%p for %d at %d %c\n", __func__,
			rc, tgid, nid, remote ? 'r' : 'l');

	return rc;
}

void exit_remote_context(struct remote_context *rc)
{
	if (!rc) return;

	printk("%s: %p\n", __func__, rc);
	// TODO: deallocate this context.
	//kfree(rc);
}



///////////////////////////////////////////////////////////////////////////////
// Marina's data stores (linked lists). remove quickly
/*
 * Functions to manipulate memory list
 * head: _memory_head
 * lock: _memory_head_lock
 */
LIST_HEAD(_memory_head);
DEFINE_SPINLOCK(_memory_head_lock);

memory_t* find_memory_entry(int cpu, int id)
{
	memory_t *m = NULL;
	memory_t *found = NULL;
	unsigned long flags;

	spin_lock_irqsave(&_memory_head_lock, flags);
	list_for_each_entry(m, &_memory_head, list) {
		if (m->tgroup_home_cpu == cpu && m->tgroup_home_id == id) {
			found = m;
			break;
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
///////////////////////////////////////////////////////////////////////////////


static void __rename_task_comm(struct task_struct *tsk, char *name)
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
	set_task_comm(tsk, comm);
}


///////////////////////////////////////////////////////////////////////////////
// Handle process/task exit
///////////////////////////////////////////////////////////////////////////////

int process_server_task_exit(struct task_struct *tsk)
{
	struct remote_context *rc;
	bool last = false;
	bool notify = false;

	if (!process_is_distributed(tsk)) return -ESRCH;

	rc = tsk->mm->remote;

	printk("EXIT [%d]: %s %s/ 0x%x 0x%x\n", tsk->pid,
			tsk->at_remote ? "remote" : "local",
			tsk->is_vma_worker ? "worker " : "",
			tsk->exit_code, tsk->group_exit);
	printk("EXIT [%d]: 0x%p %d\n", tsk->pid,
			rc, atomic_read(&rc->count));
	//__show_regs(regs, 1);

	if (tsk->is_vma_worker == 1) return 0;

	/*
	 * Normally, tsk->remote should be null upon exit unless it is terminated
	 * unexpectedly. Deal with this case
	 */
	if (tsk->remote) {
		put_task_remote(tsk);
		notify = true;
	}
	if (atomic_read(&rc->count) == 0) {
		last = (rc == cmpxchg(&tsk->mm->remote, rc, NULL));
	}

	if (tsk->at_remote) {
		if (tsk->exit_code == TASK_PARKED) {
			// Quietly exit back-migrated thread.
		} else  {
			if (last) {
				// TODO: might be redundant but safe..
				page_server_flush_remote_pages(rc);
			}

			if (notify) {
				// Notify the origin of the task exit
				task_exit_t *req = kmalloc(sizeof(*req), GFP_KERNEL);
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
			}
		}
	} else { // !tsk->at_remote
		if (tsk->remote_nid != -1 || tsk->remote_pid != -1) {
			WARN_ON("Don't forget to terminate remote tasks");
			// Send task_exit with EBUSY
		}
	}

	if (last) {
		printk("EXIT [%d]: close the remote context\n", tsk->pid);
		list_del(&rc->list);		// XXX: possible race in rc iteration
		rc->vma_worker_stop = true;
		while(!rc->vma_worker) {
			schedule();
		}
		wake_up_process(rc->vma_worker);
		complete(&rc->spawn_egg);
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

		tsk->remote = NULL;
		tsk->remote_nid = -1;
		tsk->remote_pid = -1;
		put_task_remote(tsk);

		tsk->ret_from_remote = TASK_DEAD;
		tsk->exit_code = req->exit_code;
		tsk->group_exit = req->group_exit;

		restore_thread_info(tsk, &req->arch);
		smp_wmb();

		wake_up_process(tsk);
	} else {
		printk(KERN_INFO"%s: %d not found\n", __func__, req->origin_pid);
	}

	pcn_kmsg_free_msg(req);
	kfree(work);
}



///////////////////////////////////////////////////////////////////////////////
// handling back migration
///////////////////////////////////////////////////////////////////////////////

static int handle_back_migration(struct pcn_kmsg_message *inc_msg)
{
	back_migration_request_t *req = (back_migration_request_t *)inc_msg;
	struct task_struct *tsk;

#ifdef MIGRATION_PROFILE
	migration_start = ktime_get();
#endif

	PSPRINTK("### BACKMIG from %d at %d to %d\n",
			req->remote_pid, req->remote_nid, req->origin_pid);

	rcu_read_lock();
	tsk = find_task_by_vpid(req->origin_pid);
	rcu_read_unlock();

	if (!tsk) {
		printk("%s: no origin taks %d for remote %d\n",
				__func__, req->origin_pid, req->remote_pid);
		goto out_free;
	}

	get_task_struct(tsk);

	BUG_ON(tsk->remote_pid != req->remote_pid);

	/* Welcome home */
	tsk->remote = NULL;
	tsk->remote_nid = -1;
	tsk->remote_pid = -1;
	tsk->ret_from_remote = TASK_RUNNING;
	put_task_remote(tsk);

	restore_thread_info(tsk, &req->arch);
	tsk->personality = req->personality;
	initialize_thread_retval(tsk, 0);
	smp_wmb();

	wake_up_process(tsk);
	put_task_struct(tsk);

#ifdef MIGRATION_PROFILE
	migration_end = ktime_get();
	printk(KERN_ERR"Time for back migration - origin side: %ld ns\n",
			GET_MIGRATION_TIME);
#endif

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

	BUG_ON(tsk->origin_nid == -1 && tsk->origin_pid == -1);

	req = kmalloc(sizeof(*req), GFP_KERNEL);
	BUG_ON(!req);

	/*
	 * Flush pages to the server before back-migrate this if this is
	 * the last remote at here,
	 */
	if (put_task_remote(tsk)) {
		page_server_flush_remote_pages(tsk->mm->remote);
	}

	//printk("%s entered dst{%d}\n", __func__, dst_cpu);
	req->header.type = PCN_KMSG_TYPE_PROC_SRV_BACK_MIGRATE;
	req->header.prio = PCN_KMSG_PRIO_NORMAL;

	req->origin_pid = tsk->origin_pid;
	req->remote_nid = my_nid;
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

	tsk->remote = NULL;
	tsk->origin_nid = tsk->origin_pid = -1;
	kfree(req);

#ifdef MIGRATION_PROFILE
	migration_end = ktime_get();
	printk(KERN_ERR"Time for back migration - remote side: %ld ns\n",
			GET_MIGRATION_TIME);
#endif

	do_exit(TASK_PARKED);
}



///////////////////////////////////////////////////////////////////////////////
// Pairing task pid
///////////////////////////////////////////////////////////////////////////////

static int handle_remote_task_pairing(struct pcn_kmsg_message *msg)
{
	remote_task_pairing_t *req = (remote_task_pairing_t *)msg;
	unsigned int nid_from = req->header.from_cpu;
	struct task_struct *tsk;

	rcu_read_lock();
	tsk = find_task_by_vpid(req->your_pid);
	rcu_read_unlock();
	if (tsk == NULL) {
		pcn_kmsg_free_msg(req);
		return -ESRCH;
	}
	BUG_ON(tsk->at_remote);

	tsk->remote_nid = nid_from;
	tsk->remote_pid = req->my_pid;

	/*
	PSPRINTK("Pairing local:  %d --> %d at %d\n",
			tsk->pid, nid_from, req->my_pid);
	*/

	pcn_kmsg_free_msg(req);
	return 0;
}


static int __pair_remote_task(struct task_struct *tsk)
{
	remote_task_pairing_t *req;
	int ret;

	req = kmalloc(sizeof(*req), GFP_KERNEL);
	BUG_ON(!req);

	// Notify remote cpu of pairing between current task and remote
	// representative task.
	req->header.type = PCN_KMSG_TYPE_PROC_SRV_CREATE_PROCESS_PAIRING;
	req->header.prio = PCN_KMSG_PRIO_NORMAL;
	req->my_pid = current->pid;
	req->your_pid = current->origin_pid;

	/*
	PSPRINTK("Pairing remote: %d --> %d at %d\n",
			req->my_pid, req->your_pid, current->origin_nid);
	*/

	ret = pcn_kmsg_send_long(current->origin_nid, req, sizeof(*req));
	kfree(req);

	return ret;
}



///////////////////////////////////////////////////////////////////////////////
// Remote thread
///////////////////////////////////////////////////////////////////////////////

struct shadow_params {
	clone_request_t *req;
};

static int shadow_main(void *_args)
{
	struct shadow_params *params = _args;
	clone_request_t *req = params->req;

    PSPRINTK("%s [%d]: start\n", __func__, current->pid);

	__rename_task_comm(current, req->exe_path);

	current->flags &= ~(PF_KTHREAD | PF_RANDOMIZE);	/* Convert to user */
	current->origin_nid = req->origin_nid;
	current->origin_pid = req->origin_pid;
	current->personality = req->personality;
	current->at_remote = true;
	current->remote = get_task_remote(current);

	// set thread info
	restore_thread_info(current, &req->arch);
	initialize_thread_retval(current, 0);

	/*
	sigorsets(&current->blocked, &current->blocked, &req->remote_blocked);
	sigorsets(&current->real_blocked,
			&current->real_blocked, &req->remote_real_blocked);
	sigorsets(&current->saved_sigmask,
			&current->saved_sigmask, &req->remote_saved_sigmask);
	current->pending = req->remote_pending;
	current->sas_ss_sp = req->sas_ss_sp;
	current->sas_ss_size = req->sas_ss_size;
	memcpy(current->sighand->action, req->action, sizeof(req->action));
	*/

	__pair_remote_task(current);

#ifdef MIGRATION_PROFILE
	migration_end = ktime_get();
	printk(KERN_ERR"Time for migration - remote side: %ld ns\n",
			GET_MIGRATION_TIME);
#endif

	printk("####### MIGRATED - %d at %d --> %d at %d\n",
			current->origin_pid, current->origin_nid, current->pid, my_nid);
	/*
	printk("%s: pc %lx ip %lx\n", __func__,
			(&req->arch)->migration_pc, current_pt_regs()->ip);
	printk("%s: sp %lx bp %lx\n", __func__,
			current_pt_regs()->sp, current_pt_regs()->bp);
	*/

	kfree(params);
	pcn_kmsg_free_msg(req);

	update_thread_info();

	return 0;	/* Returning from here will start the user thread run */
}


static void __kick_shadow_spawner(struct remote_context *rc, struct pcn_kmsg_work *work)
{
	/* Utilize the list_head in work_struct */
	struct list_head *entry = &((struct work_struct *)work)->entry;

	INIT_LIST_HEAD(entry);
	spin_lock(&rc->shadow_eggs_lock);
	list_add(entry, &rc->shadow_eggs);
	spin_unlock(&rc->shadow_eggs_lock);

	complete(&rc->spawn_egg);
}


int shadow_spawner(void *_args)
{
	struct remote_context *rc = _args;
	rc->shadow_spawner = current;
	smp_wmb();

	PSPRINTK(KERN_INFO"%s [%d]: started\n", __func__, current->pid);
	current->is_vma_worker = true;

	while (!rc->vma_worker_stop) {
		struct work_struct *work = NULL;
		struct shadow_params *params;

		wait_for_completion(&rc->spawn_egg);

		spin_lock(&rc->shadow_eggs_lock);
		if (!list_empty(&rc->shadow_eggs)) {
			work = list_first_entry(
					&rc->shadow_eggs, struct work_struct, entry);
			list_del(&work->entry);
		}
		spin_unlock(&rc->shadow_eggs_lock);

		if (!work) continue;

		params = kmalloc(sizeof(*params), GFP_KERNEL);
		params->req = ((struct pcn_kmsg_work *)work)->msg;

		/* Following loop deals with signals between concurrent migration */
		while (kernel_thread(shadow_main, params,
						CLONE_THREAD | CLONE_SIGHAND | SIGCHLD) < 0) {
			schedule();
		}
		kfree(work);
	}

	PSPRINTK(KERN_INFO"%s [%d]: exited\n", __func__, current->pid);

	do_exit(0);
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

static int vma_worker_remote(void *_data)
{
	struct vma_worker_params *params = (struct vma_worker_params *)_data;
	struct pcn_kmsg_work *work = params->work;
	clone_request_t *req = work->msg;
	struct remote_context *rc = params->rc;
	int origin_tgid = req->origin_tgid;
	pid_t pid;

	might_sleep();

	PSPRINTK("%s [%d]: origin nid=%d tgid=%d pid=%d\n", __func__,
			current->pid, req->origin_nid, req->origin_tgid, req->origin_pid);
	PSPRINTK("%s [%d]: exe_path=%s\n", __func__,
			current->pid, req->exe_path);

	if (__construct_vma_worker_mm(req, rc) != 0) {
		BUG();
		return -EINVAL;
	}

	current->is_vma_worker = true;
	current->at_remote = true;
	current->origin_nid = req->origin_nid;
	current->origin_pid = req->origin_pid;

	/* Create the shadow spawner */
	pid = kernel_thread(shadow_spawner, rc,
			CLONE_THREAD | CLONE_SIGHAND | SIGCHLD);
	BUG_ON(pid < 0);

	rc->tgid = current->tgid;
	rc->remote_tgids[my_nid] = rc->tgid;
	rc->remote_tgids[current->origin_nid] = origin_tgid;
	rc->vma_worker = current;
	smp_wmb();

	list_add(&rc->list, &__remote_contexts_in());
	__unlock_remote_contexts_in(nid_from);

	kfree(params);
	vma_worker_main(rc, "remote");

	return 0;
}


static void clone_remote_thread(struct work_struct *_work)
{
	struct pcn_kmsg_work *work = (struct pcn_kmsg_work *)_work;
	clone_request_t *req = work->msg;
	int nid_from = req->origin_nid;
	int tgid_from = req->origin_tgid;
	struct remote_context *rc;
	struct remote_context *rc_new =
			__alloc_remote_context(nid_from, tgid_from, true);
	struct vma_worker_params *params = kmalloc(sizeof(*params), GFP_KERNEL);
	char *which_rc;

	BUG_ON(!rc_new || !params);

	__lock_remote_contexts_in(nid_from);
	rc = __lookup_remote_contexts_in(nid_from, tgid_from);
	if (!rc) {
		params->rc = rc = rc_new;
		params->work = work;

		smp_wmb();
		/*
		 * Start the helper thread. remote_contexts_lock is released upon
		 * thread creation completion
		 */
		kthread_run(vma_worker_remote, params, "vma_worker_remote");
		which_rc = "created";
	} else {
		__unlock_remote_contexts_in(nid_from);
		kfree(rc_new);
		which_rc = "found";
	}

	PSPRINTK("%s: remote_context %s at 0x%p\n", __func__, which_rc, rc);

	// Kick the spawner
	__kick_shadow_spawner(rc, work);
	return;
}


static int handle_clone_request(struct pcn_kmsg_message *msg)
{
	clone_request_t *req = (clone_request_t *)msg;
	struct pcn_kmsg_work *work = kmalloc(sizeof(*work), GFP_ATOMIC);
	BUG_ON(!work);

#ifdef MIGRATION_PROFILE
	migration_start = ktime_get();
#endif

	work->msg = req;
	INIT_WORK((struct work_struct *)work, clone_remote_thread);
	queue_work(clone_wq, (struct work_struct *)work);

	return 0;
}


/**
 * Send a message to <dst_nid> for migrating a task <task>.
 * This function will ask the remote node to create a thread to host the task.
 * It returns <0 in error case.
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
	req->origin_nid = my_nid;
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


static int vma_worker_origin(void *_arg)
{
	struct remote_context *rc = _arg;

	might_sleep();
	current->is_vma_worker = true;
	current->at_remote = false;
	rc->vma_worker = current;
	smp_wmb();

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
	rc_new = __alloc_remote_context(my_nid, tsk->tgid, false);

	/* I use siglock to coordinate the thread group. */
	lock_task_sighand(tsk, &flags);
	if (tsk->mm->remote) {
		kfree(rc_new);
	} else {
		/*
		 * This process is becoming a distributed one if it was not already.
		 * The first migrating thread attaches the remote context to
		 * mm->remote, which indicates this process is distributed, and
		 * forks the vma worker thread for this process.
		 */

		/*
		 * Setting mm->remote to remote_context indicates
		 * this process is distributed
		 */
		tsk->mm->remote = rc_new;
		rc_new->mm = tsk->mm;
		rc_new->remote_tgids[my_nid] = tsk->tgid;
		smp_wmb();

		__lock_remote_contexts_out(dst_nid);
		list_add(&rc_new->list, &__remote_contexts_out());
		__unlock_remote_contexts_out(dst_nid);

		create_vma_worker = true;
	}
	rc = get_task_remote(tsk);
	tsk->remote = rc;
	tsk->at_remote = false;
	unlock_task_sighand(tsk, &flags);

	if (create_vma_worker) {
		pid_t pid = kernel_thread_popcorn(vma_worker_origin, rc);
		BUG_ON(pid < 0);
	}

	ret = __request_clone_remote(dst_nid, tsk, regs, uregs);

#ifdef MIGRATION_PROFILE
	migration_end = ktime_get();
	printk(KERN_ERR"Time for migration - origin side: %ld ns\n",
			GET_MIGRATION_TIME);
#endif

	return ret;
}


/**
 * Migrate the specified task <task> to node <dst_nid>
 * Currently, this function will put the specified task to sleep,
 * and push its info over to the remote cpu.
 * The remote cpu will then create a new thread and import that
 * info into its new context.
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
		if (ret < 0) {
			put_task_remote(tsk);
		}
	}

	return ret;
}


DEFINE_KMSG_WQ_HANDLER(exit_task, exit_wq);

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

	INIT_LIST_HEAD(&remote_contexts[0]);
	INIT_LIST_HEAD(&remote_contexts[1]);

	spin_lock_init(&remote_contexts_lock[0]);
	spin_lock_init(&remote_contexts_lock[1]);

	/* Register handlers */
	REGISTER_KMSG_HANDLER(
			PCN_KMSG_TYPE_PROC_SRV_MIGRATE, clone_request);
	REGISTER_KMSG_HANDLER(
			PCN_KMSG_TYPE_PROC_SRV_BACK_MIGRATE, back_migration);
	REGISTER_KMSG_HANDLER(
			PCN_KMSG_TYPE_PROC_SRV_CREATE_PROCESS_PAIRING, remote_task_pairing);

	REGISTER_KMSG_WQ_HANDLER(
			PCN_KMSG_TYPE_PROC_SRV_TASK_EXIT, exit_task);

	return 0;
}
