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
#include <linux/ptrace.h>
#include <linux/mmu_context.h>
#include <linux/fs.h>

#include <asm/mmu_context.h>
#include <asm/kdebug.h>
#include <asm/uaccess.h>

#include <popcorn/bundle.h>
#include <popcorn/cpuinfo.h>
#include <popcorn/debug.h>
#include <popcorn/process_server.h>

#include "types.h"
#include "vma_server.h"
#include "page_server.h"
#include "stat.h"


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
	struct remote_context *rc = kmalloc(sizeof(*rc), GFP_KERNEL);
	BUG_ON(!rc);

	INIT_LIST_HEAD(&rc->list);
	atomic_set(&rc->count, 0);
	rc->mm = NULL;

	rc->tgid = tgid;
	rc->for_remote = remote;

	INIT_LIST_HEAD(&rc->pages);
	spin_lock_init(&rc->pages_lock);

	INIT_LIST_HEAD(&rc->vmas);
	spin_lock_init(&rc->vmas_lock);

	rc->vma_worker = NULL;
	rc->vma_worker_stop = false;

	rc->shadow_spawner = NULL;
	INIT_LIST_HEAD(&rc->shadow_eggs);
	spin_lock_init(&rc->shadow_eggs_lock);
	init_completion(&rc->spawn_egg);

	memset(rc->remote_tgids, 0x00, sizeof(rc->remote_tgids));

	barrier();

	/*
	printk(KERN_INFO"%s: at 0x%p for %d at %d %c\n", __func__,
			rc, tgid, nid, remote ? 'r' : 'l');
	*/

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

static void __build_task_comm(char *buffer, char *path)
{
	int i, ch;
	for (i = 0; (ch = *(path++)) != '\0';) {
		if (ch == '/')
			i = 0;
		else if (i < (TASK_COMM_LEN - 1))
			buffer[i++] = ch;
	}
	buffer[i] = '\0';
}


///////////////////////////////////////////////////////////////////////////////
// Handle process/task exit
///////////////////////////////////////////////////////////////////////////////

int process_server_task_exit(struct task_struct *tsk)
{
	struct remote_context *rc;
	bool last = false;
	bool notify = false;

	WARN_ON(tsk != current);

	if (!process_is_distributed(tsk)) return -ESRCH;

	rc = tsk->mm->remote;

	PSPRINTK("EXIT [%d]: %s / 0x%x\n", tsk->pid,
			tsk->at_remote ? "remote" : "local", tsk->exit_code);
	/*
	printk("EXIT [%d]: 0x%p %d\n", tsk->pid, rc, atomic_read(&rc->count));
	__show_regs(regs, 1);
	*/

	BUG_ON(tsk->is_vma_worker);

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
		} else if (notify) {
			task_exit_t *req = kmalloc(sizeof(*req), GFP_KERNEL);
			BUG_ON(!req);

			if (last) {
				// might be redundant but safe..
				page_server_flush_remote_pages(rc);
			}

			// Notify the origin of the task exit
			req->header.type = PCN_KMSG_TYPE_TASK_EXIT;
			req->header.prio = PCN_KMSG_PRIO_NORMAL;

			req->origin_pid = tsk->origin_pid;
			req->remote_pid = tsk->pid;
			req->exit_code = tsk->exit_code;

			req->expect_flush = last;

			save_thread_info(tsk, &req->arch, NULL);

			pcn_kmsg_send_long(tsk->origin_nid, req, sizeof(*req));
			kfree(req);
		}
	} else { // !tsk->at_remote
		if (tsk->remote_nid != -1 || tsk->remote_pid != -1) {
			WARN_ON("Don't forget to terminate remote tasks");
		}
	}

	if (last) {
		PSPRINTK("EXIT [%d]: finish at %s\n",
				tsk->pid, tsk->at_remote ? "remote" : "local");
		__lock_remote_contexts(tsk->at_remote);
		list_del(&rc->list);
		__unlock_remote_contexts(tsk->at_remote);
		kthread_stop(rc->vma_worker);
		rc->vma_worker_stop = true;
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

	tsk = __get_task_struct(req->origin_pid);
	if (!tsk) {
		printk(KERN_INFO"%s: %d not found\n", __func__, req->origin_pid);
		goto out;
	}

	if (tsk->remote_pid == req->remote_pid) {
		printk(KERN_INFO"%s [%d]: exited with 0x%lx\n", __func__,
				tsk->pid, req->exit_code);

		if (req->expect_flush) {
			wait_for_completion(&tsk->wait_for_remote_flush);
		}
		tsk->remote = NULL;
		tsk->remote_nid = -1;
		tsk->remote_pid = -1;
		put_task_remote(tsk);

		tsk->ret_from_remote = TASK_DEAD;
		tsk->exit_code = req->exit_code;

		restore_thread_info(tsk, &req->arch, false);
		smp_mb();

		wake_up_process(tsk);
	} else {
		printk(KERN_INFO"%s: %d not found\n", __func__, req->origin_pid);
	}
	put_task_struct(tsk);

out:
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

	tsk = __get_task_struct(req->origin_pid);
	if (!tsk) {
		printk("%s: no origin taks %d for remote %d\n",
				__func__, req->origin_pid, req->remote_pid);
		goto out_free;
	}

	PSPRINTK("### BACKMIG [%d] from %d at %d\n",
			tsk->pid, req->remote_pid, req->remote_nid);

	BUG_ON(tsk->remote_pid != req->remote_pid);

	/* Welcome home */
	if (req->expect_flush) {
		wait_for_completion(&tsk->wait_for_remote_flush);
	}
	tsk->remote = NULL;
	tsk->remote_nid = -1;
	tsk->remote_pid = -1;
	put_task_remote(tsk);

	tsk->ret_from_remote = TASK_RUNNING;
	tsk->personality = req->personality;

	restore_thread_info(tsk, &req->arch, false);
	smp_mb();

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
		req->expect_flush = true;
		page_server_flush_remote_pages(tsk->mm->remote);
	} else {
		req->expect_flush = false;
	}

	//printk("%s entered dst{%d}\n", __func__, dst_cpu);
	req->header.type = PCN_KMSG_TYPE_TASK_MIGRATE_BACK;
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

	save_thread_info(tsk, &req->arch, uregs);

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
	struct task_struct *tsk;
	int ret = 0;

	tsk = __get_task_struct(req->your_pid);
	if (!tsk) {
		ret = -ESRCH;
		goto out;
	}
	BUG_ON(tsk->at_remote);
	BUG_ON(!tsk->remote);

	tsk->remote_nid = req->my_nid;
	tsk->remote_pid = req->my_pid;
	tsk->remote->remote_tgids[req->my_nid] = req->my_tgid;

	/*
	PSPRINTK("Pairing local:  %d --> %d at %d\n",
			tsk->pid, req->my_nid, req->my_pid);
	*/

	put_task_struct(tsk);
out:
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
	req->header.type = PCN_KMSG_TYPE_TASK_PAIRING;
	req->header.prio = PCN_KMSG_PRIO_NORMAL;
	req->my_nid = my_nid;
	req->my_tgid = current->tgid;
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

    PSPRINTK("%s [%d]: started for %d at %d\n", __func__,
			current->pid, req->origin_pid, req->origin_nid);

	current->flags &= ~PF_KTHREAD;	/* Drop to user */
	current->origin_nid = req->origin_nid;
	current->origin_pid = req->origin_pid;
	current->at_remote = true;
	current->remote = get_task_remote(current);

	set_fs(USER_DS);

	/* Inject thread info here */
	restore_thread_info(current, &req->arch, true);

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

	PSPRINTK("\n####### MIGRATED - %d at %d --> %d at %d\n",
			current->origin_pid, current->origin_nid, current->pid, my_nid);

	kfree(params);
	pcn_kmsg_free_msg(req);

	return 0;
	/* Returning from here makes this thread jump into the user-space */
}


static void __kick_shadow_spawner(struct remote_context *rc, struct pcn_kmsg_work *work)
{
	/* Exploit the list_head in work_struct */
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

	PSPRINTK(KERN_INFO"%s [%d]: started\n", __func__, current->pid);

	current->is_vma_worker = true;
	rc->shadow_spawner = current;

	while (!rc->vma_worker_stop) {
		struct work_struct *work = NULL;
		struct shadow_params *params;

		wait_for_completion_timeout(&rc->spawn_egg, 10 * HZ);

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


static int __construct_mm(clone_request_t *req, struct remote_context *rc)
{
	struct mm_struct *mm;
	struct file *f;

	mm = mm_alloc();
	if (!mm) {
		BUG_ON(!mm);
		return -ENOMEM;
	}

	arch_pick_mmap_layout(mm);

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

	mm->distr_vma_op_counter = 0;
	mm->was_not_pushed = 0;
	mm->thread_op = NULL;
	mm->vma_operation_index = 0;
	mm->distribute_unmap = 1;

	use_mm(mm);

	rc->mm = mm;
	mm->remote = rc;

	return 0;
}


struct vma_worker_params {
	struct pcn_kmsg_work *work;
	struct remote_context *rc;
	char comm[TASK_COMM_LEN];
};

static int vma_worker_remote(void *_data)
{
	struct vma_worker_params *params = (struct vma_worker_params *)_data;
	struct pcn_kmsg_work *work = params->work;
	clone_request_t *req = work->msg;
	struct remote_context *rc = params->rc;
	struct cred *new;

	might_sleep();

	PSPRINTK("%s [%d]: started for origin %d in %d at %d\n", __func__,
			current->pid, req->origin_pid, req->origin_tgid, req->origin_nid);
	PSPRINTK("%s [%d]: exe_path=%s\n", __func__,
			current->pid, req->exe_path);

	current->flags &= ~PF_RANDOMIZE;	/* Disable ASLR for now*/
	current->personality = req->personality;
	current->is_vma_worker = true;
	current->at_remote = true;
	current->origin_nid = req->origin_nid;
	current->origin_pid = req->origin_pid;

	set_user_nice(current, 0);
	new = prepare_kernel_cred(current);
	commit_creds(new);

	if (__construct_mm(req, rc) != 0) {
		BUG();
		return -EINVAL;
	}

	rc->tgid = current->tgid;
	smp_mb();

	/* Create the shadow spawner */
	kernel_thread(shadow_spawner, rc, CLONE_THREAD | CLONE_SIGHAND | SIGCHLD);

	/* Drop to user here to access mm using get_task_mm().
	 * This should be done after forking shadow_spawner otherwise
	 * kernel_thread() will consider this as a user thread fork() which
	 * will end up an inproper instruction pointer (see copy_tls_copy()).
	 */
	current->flags &= ~PF_KTHREAD;

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

	BUG_ON(!rc_new);

	PSPRINTK("%s: for %d in %d at %d\n", __func__,
			req->origin_pid, tgid_from, nid_from);

	__lock_remote_contexts_in(nid_from);
	rc = __lookup_remote_contexts_in(nid_from, tgid_from);
	if (!rc) {
		struct vma_worker_params *params;

		rc = rc_new;
		rc->remote_tgids[nid_from] = tgid_from;
		smp_mb();
		list_add(&rc->list, &__remote_contexts_in());
		__unlock_remote_contexts_in(nid_from);

		params = kmalloc(sizeof(*params), GFP_KERNEL);
		BUG_ON(!params);

		params->rc = rc;
		params->work = work;
		__build_task_comm(params->comm, req->exe_path);
		smp_mb();

		rc->vma_worker =
				kthread_run(vma_worker_remote, params, params->comm);
	} else {
		__unlock_remote_contexts_in(nid_from);
		kfree(rc_new);
	}

	/* Kick the spawner */
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
	queue_work(popcorn_wq, (struct work_struct *)work);

	return 0;
}


/**
 * Send a message to <dst_nid> for migrating a task <task>.
 * This function will ask the remote node to create a thread to host the task.
 * It returns <0 in error case.
 */
static int __request_clone_remote(int dst_nid, struct task_struct *tsk, void __user *uregs)
{
	char *rpath, path[512];
	struct mm_struct *mm = get_task_mm(tsk);
	clone_request_t *req;
	int ret;

	might_sleep();

	req = kmalloc(sizeof(*req), GFP_KERNEL);
	BUG_ON(!req);

	/* Build request */
	req->header.type = PCN_KMSG_TYPE_TASK_MIGRATE;
	req->header.prio = PCN_KMSG_PRIO_NORMAL;

	/* struct mm_struct */
	rpath = d_path(&mm->exe_file->f_path, path, sizeof(path));
	if (IS_ERR(rpath)) {
		printk("%s: exe binary path is too long.\n", __func__);
		ret = -ESRCH;
		goto out;
	} else {
		strncpy(req->exe_path, rpath, sizeof(req->exe_path));
	}

	req->stack_start = mm->start_stack;
	req->start_brk = mm->start_brk;
	req->brk = mm->brk;
	req->env_start = mm->env_start;
	req->env_end = mm->env_end;
	req->arg_start = mm->arg_start;
	req->arg_end = mm->arg_end;
	req->start_code = mm->start_code;
	req->end_code = mm->end_code;
	req->start_data = mm->start_data;
	req->end_data = mm->end_data;
	req->def_flags = mm->def_flags;

	/* struct tsk_struct */
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

	save_thread_info(tsk, &req->arch, uregs);

	ret = pcn_kmsg_send_long(dst_nid, req, sizeof(*req));

out:
	kfree(req);
	mmput(mm);

	return ret;
}


static int vma_worker_origin(void *_arg)
{
	struct remote_context *rc = _arg;

	might_sleep();
	current->is_vma_worker = true;
	current->at_remote = false;
	use_mm(rc->mm);

	vma_worker_main(rc, "local");

	return 0;
}


int do_migration(struct task_struct *tsk, int dst_nid, void __user *uregs)
{
	int ret;
	unsigned long flags;
	bool create_vma_worker = false;
	struct remote_context *rc, *rc_new;
	char *which_rc;

	might_sleep();

	/* Want to avoid allocate this structure in the spinlock-ed area */
	rc_new = __alloc_remote_context(my_nid, tsk->tgid, false);

	/* Use siglock to coordinate the thread group. */
	lock_task_sighand(tsk, &flags);
	if (tsk->mm->remote) {
		kfree(rc_new);
		which_rc = "found";
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

		__lock_remote_contexts_out(dst_nid);
		list_add(&rc_new->list, &__remote_contexts_out());
		__unlock_remote_contexts_out(dst_nid);

		create_vma_worker = true;
		which_rc = "allocated";
	}
	rc = get_task_remote(tsk);
	tsk->remote = rc;
	tsk->at_remote = false;
	unlock_task_sighand(tsk, &flags);

	if (create_vma_worker) {
		rc->vma_worker = kthread_run(vma_worker_origin, rc, "worker_origin");
	}
	PSPRINTK("%s [%d]: remote context %s\n", __func__, tsk->pid, which_rc);

	ret = __request_clone_remote(dst_nid, tsk, uregs);

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
int process_server_do_migration(struct task_struct *tsk, unsigned int dst_nid, void __user *uregs)
{
	int ret = 0;

	if (tsk->origin_nid == dst_nid) {
		ret = do_back_migration(tsk, dst_nid, uregs);
	} else {
		ret = do_migration(tsk, dst_nid, uregs);
		if (ret < 0) {
			tsk->remote = NULL;
			tsk->remote_pid = tsk->remote_nid = -1;
			put_task_remote(tsk);
		}
	}

	return ret;
}


DEFINE_KMSG_WQ_HANDLER(exit_task);

/**
 * Initialize the process server.
 */
int __init process_server_init(void)
{
	INIT_LIST_HEAD(&remote_contexts[0]);
	INIT_LIST_HEAD(&remote_contexts[1]);

	spin_lock_init(&remote_contexts_lock[0]);
	spin_lock_init(&remote_contexts_lock[1]);

	/* Register handlers */
	REGISTER_KMSG_HANDLER(PCN_KMSG_TYPE_TASK_MIGRATE, clone_request);
	REGISTER_KMSG_HANDLER(PCN_KMSG_TYPE_TASK_MIGRATE_BACK, back_migration);
	REGISTER_KMSG_HANDLER(PCN_KMSG_TYPE_TASK_PAIRING, remote_task_pairing);

	REGISTER_KMSG_WQ_HANDLER(
			PCN_KMSG_TYPE_TASK_EXIT, exit_task);

	return 0;
}
