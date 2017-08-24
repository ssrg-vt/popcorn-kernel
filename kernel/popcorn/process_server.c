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

#include <popcorn/types.h>
#include <popcorn/bundle.h>
#include <popcorn/cpuinfo.h>

#include "types.h"
#include "process_server.h"
#include "vma_server.h"
#include "page_server.h"
#include "util.h"

static struct list_head remote_contexts[2];
static spinlock_t remote_contexts_lock[2];

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


inline struct remote_context *get_task_remote(struct task_struct *tsk)
{
	struct remote_context *rc = tsk->mm->remote;
	atomic_inc(&rc->count);

	return rc;
}

inline bool __put_task_remote(struct remote_context *rc)
{
	if (unlikely(!atomic_dec_and_test(&rc->count)))
		return false;

	__lock_remote_contexts(rc->for_remote);
	if (atomic_read(&rc->count)) {
		BUG();
		__unlock_remote_contexts(rc->for_remote);
		return false;
	}
	list_del(&rc->list);
	__unlock_remote_contexts(rc->for_remote);

	mmput(rc->mm);
	kfree(rc);
	return true;
}

inline bool put_task_remote(struct task_struct *tsk)
{
	return __put_task_remote(tsk->mm->remote);
}

static struct remote_context *__alloc_remote_context(int nid, int tgid, bool remote)
{
	struct remote_context *rc = kmalloc(sizeof(*rc), GFP_KERNEL);
	BUG_ON(!rc);

	INIT_LIST_HEAD(&rc->list);
	atomic_set(&rc->count, 1);
	rc->mm = NULL;

	rc->tgid = tgid;
	rc->for_remote = remote;

	INIT_LIST_HEAD(&rc->faults);
	spin_lock_init(&rc->faults_lock);

	INIT_LIST_HEAD(&rc->vmas);
	spin_lock_init(&rc->vmas_lock);

	rc->vma_worker_stop = false;

	rc->vma_worker = NULL;
	INIT_LIST_HEAD(&rc->vma_works);
	spin_lock_init(&rc->vma_works_lock);
	init_completion(&rc->vma_works_ready);

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
static void __terminate_peers(struct remote_context *rc)
{
	int nid;
	origin_task_exit_t req = {
		.header.type = PCN_KMSG_TYPE_TASK_EXIT_ORIGIN,
		.header.prio = PCN_KMSG_PRIO_NORMAL,

		.origin_pid = current->pid,
		.exit_code = current->exit_code,
	};

	/* Take down peer vma workers */
	for (nid = 0; nid < MAX_POPCORN_NODES; nid++) {
		if (nid == my_nid || rc->remote_tgids[nid] == 0) continue;
		PSPRINTK("EXIT [%d] terminate [%d/%d] %d\n", current->pid,
				rc->remote_tgids[nid], nid, req.exit_code);

		req.remote_pid = rc->remote_tgids[nid];
		pcn_kmsg_send(nid, &req, sizeof(req));
	}
}

static int __exit_origin_task(struct task_struct *tsk)
{
	struct remote_context *rc = tsk->mm->remote;

	if (tsk->remote) {
		put_task_remote(tsk);
	}
	tsk->remote = NULL;
	tsk->origin_nid = tsk->origin_pid = -1;

	/**
	 * Trigger peer termination if this is the last user thread
	 * referring to this mm.
	 * 2 == current + remote_context
	 */
	if (atomic_read(&tsk->mm->mm_users) == 2) {
		__terminate_peers(rc);
		__put_task_remote(rc);
	}

	return 0;
}

static int __exit_remote_task(struct task_struct *tsk)
{
	if (tsk->exit_code == TASK_PARKED) {
		/* Skip notifying for back-migrated threads */
	} else {
		/* Something went south. Notify the origin. */
		if (!get_task_remote(tsk)->vma_worker_stop) {
			remote_task_exit_t req = {
				.header.type = PCN_KMSG_TYPE_TASK_EXIT_REMOTE,
				.header.prio = PCN_KMSG_PRIO_NORMAL,

				.origin_pid = tsk->origin_pid,
				.remote_pid = tsk->pid,
				.exit_code = tsk->exit_code,
			};
			pcn_kmsg_send(tsk->origin_nid, &req, sizeof(req));
		}
		put_task_remote(tsk);
	}

	put_task_remote(tsk);
	tsk->remote = NULL;
	tsk->origin_nid = tsk->origin_pid = -1;

	return 0;
}

int process_server_task_exit(struct task_struct *tsk)
{
	WARN_ON(tsk != current);

	if (!process_is_distributed(tsk)) return -ESRCH;

	PSPRINTK("EXITED [%d] %s%s / 0x%x\n", tsk->pid,
			tsk->at_remote ? "remote" : "local",
			tsk->is_vma_worker ? " worker": "",
			tsk->exit_code);

#ifdef CONFIG_POPCORN_DEBUG_PROCESS_SERVER
	show_regs(task_pt_regs(tsk));
#endif

	if (tsk->is_vma_worker) return 0;

	if (tsk->at_remote) {
		return __exit_remote_task(tsk);
	} else {
		return __exit_origin_task(tsk);
	}
}


/**
 * Handle the notification of the task kill at the remote.
 */
static void process_remote_task_exit(struct pcn_kmsg_message *msg)
{
	remote_task_exit_t *req = (remote_task_exit_t *)msg;
	struct task_struct *tsk = current;
	int exit_code = req->exit_code;

	if (tsk->remote_pid != req->remote_pid) {
		printk(KERN_INFO"%s: pid mismatch %d != %d\n", __func__,
				tsk->remote_pid, req->remote_pid);
		pcn_kmsg_free_msg(req);
		return;
	}

	PSPRINTK("%s [%d] 0x%x\n", __func__, tsk->pid, req->exit_code);

	tsk->remote = NULL;
	tsk->remote_nid = -1;
	tsk->remote_pid = -1;
	put_task_remote(tsk);

	exit_code = req->exit_code;
	pcn_kmsg_free_msg(req);

	if (exit_code & 0xff) {
		force_sig(exit_code & 0xff, tsk);
	}
	do_exit(exit_code);
}

static int handle_origin_task_exit(struct pcn_kmsg_message *msg)
{
	origin_task_exit_t *req = (origin_task_exit_t *)msg;
	struct task_struct *tsk;
	struct remote_context *rc;

	tsk = __get_task_struct(req->remote_pid);
	if (!tsk) {
		printk(KERN_INFO"%s: task %d not found\n", __func__, req->remote_pid);
		goto out;
	}
	PSPRINTK("\nTERMINATE [%d] with 0x%x\n", tsk->pid, req->exit_code);
	BUG_ON(!tsk->is_vma_worker);
	tsk->exit_code = req->exit_code;

	rc = get_task_remote(tsk);
	rc->vma_worker_stop = true;
	smp_mb();
	complete(&rc->vma_works_ready);
	complete(&rc->spawn_egg);

	put_task_remote(tsk);
	put_task_struct(tsk);
out:
	pcn_kmsg_free_msg(req);
	return 0;
}


///////////////////////////////////////////////////////////////////////////////
// handling back migration
///////////////////////////////////////////////////////////////////////////////
static void bring_back_remote_thread(struct pcn_kmsg_message *msg)
{
	back_migration_request_t *req = (back_migration_request_t *)msg;

	if (current->remote_pid != req->remote_pid) {
		printk(KERN_INFO"%s: pid mismatch during back migration (%d != %d)\n",
				__func__, current->remote_pid, req->remote_pid);
		goto out_free;
	}

	PSPRINTK("\n### BACKMIG [%d] from %d at %d\n",
			current->pid, req->remote_pid, req->remote_nid);

	/* Welcome home */

	current->remote = NULL;
	current->remote_nid = -1;
	current->remote_pid = -1;
	put_task_remote(current);

	current->personality = req->personality;

	/* XXX signals */

	restore_thread_info(current, &req->arch, false);

out_free:
	pcn_kmsg_free_msg(msg);
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

	req->header.type = PCN_KMSG_TYPE_TASK_MIGRATE_BACK;
	req->header.prio = PCN_KMSG_PRIO_NORMAL;

	req->origin_pid = tsk->origin_pid;
	req->remote_nid = my_nid;
	req->remote_pid = tsk->pid;

	req->personality = tsk->personality;

	/*
	req->remote_blocked = tsk->blocked;
	req->remote_real_blocked = tsk->real_blocked;
	req->remote_saved_sigmask = tsk->saved_sigmask;
	req->remote_pending = tsk->pending;
	req->sas_ss_sp = tsk->sas_ss_sp;
	req->sas_ss_size = tsk->sas_ss_size;
	memcpy(req->action, tsk->sighand->action, sizeof(req->action));
	*/

	ret = copy_from_user(&req->arch.regsets, uregs,
			regset_size(get_popcorn_node_arch(dst_nid)));
	BUG_ON(ret != 0);

	save_thread_info(tsk, &req->arch);

	ret = pcn_kmsg_send(dst_nid, req, sizeof(*req));

	kfree(req);
	do_exit(TASK_PARKED);
}


///////////////////////////////////////////////////////////////////////////////
// Remote thread
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

	put_task_struct(tsk);
out:
	pcn_kmsg_free_msg(req);
	return 0;
}

static int __pair_remote_task(void)
{
	remote_task_pairing_t req = {
		.header = {
			.type = PCN_KMSG_TYPE_TASK_PAIRING,
			.prio = PCN_KMSG_PRIO_NORMAL,
		},
		.my_nid = my_nid,
		.my_tgid = current->tgid,
		.my_pid = current->pid,
		.your_pid = current->origin_pid,
	};
	return pcn_kmsg_send(current->origin_nid, &req, sizeof(req));
}


struct shadow_params {
	clone_request_t *req;
};

static int shadow_main(void *_args)
{
	struct shadow_params *params = _args;
	clone_request_t *req = params->req;

	PSPRINTK("%s [%d] started for [%d/%d]\n", __func__,
			current->pid, req->origin_pid, req->origin_nid);

	current->flags &= ~PF_KTHREAD;	/* Drop to user */
	current->origin_nid = req->origin_nid;
	current->origin_pid = req->origin_pid;
	current->at_remote = true;
	current->remote = get_task_remote(current);

	set_fs(USER_DS);

	/* Inject thread info here */
	restore_thread_info(current, &req->arch, true);

	/* XXX: Skip restoring signals and handlers for now
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

	__pair_remote_task();

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
	struct remote_context *rc = get_task_remote(current);

	PSPRINTK("%s [%d] started\n", __func__, current->pid);

	current->is_vma_worker = true;
	rc->shadow_spawner = current;

	while (!rc->vma_worker_stop) {
		struct work_struct *work = NULL;
		struct shadow_params *params;

		wait_for_completion_interruptible_timeout(&rc->spawn_egg, HZ);

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

	PSPRINTK("%s [%d] exiting\n", __func__, current->pid);

	put_task_remote(current);
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

	use_mm(mm);

	rc->mm = mm;  /* No need to increase mm_users due to mm_alloc() */
	mm->remote = rc;

	return 0;
}


struct vma_worker_params {
	struct pcn_kmsg_work *work;
	struct remote_context *rc;
	char comm[TASK_COMM_LEN];
};

static void __terminate_remote_threads(struct remote_context *rc)
{
	struct task_struct *tsk;

	/* Terminate userspace threads. Tried to use do_group_exit() but it
	 * didn't work */
	rcu_read_lock();
	for_each_thread(current, tsk) {
		if (tsk->is_vma_worker) continue;
		force_sig(SIGKILL, tsk);
		break;
	}
	rcu_read_unlock();
}

static int start_vma_worker_remote(void *_data)
{
	struct vma_worker_params *params = (struct vma_worker_params *)_data;
	struct pcn_kmsg_work *work = params->work;
	clone_request_t *req = work->msg;
	struct remote_context *rc = params->rc;
	struct cred *new;

	might_sleep();
	kfree(params);

	PSPRINTK("%s [%d] for [%d/%d]\n", __func__,
			current->pid, req->origin_tgid, req->origin_nid);
	PSPRINTK("%s [%d] %s\n", __func__,
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

	get_task_remote(current);
	rc->tgid = current->tgid;
	smp_mb();

	/* Create the shadow spawner */
	kernel_thread(shadow_spawner, rc, CLONE_THREAD | CLONE_SIGHAND | SIGCHLD);

	/* Drop to user here to access mm using get_task_mm() in vma_worker routine.
	 * This should be done after forking shadow_spawner otherwise
	 * kernel_thread() will consider this as a user thread fork() which
	 * will end up an inproper instruction pointer (see copy_thread_tls()).
	 */
	current->flags &= ~PF_KTHREAD;

	vma_worker_remote(rc);
	__terminate_remote_threads(rc);

	put_task_remote(current);
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
				kthread_run(start_vma_worker_remote, params, params->comm);
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

	work->msg = req;
	INIT_WORK((struct work_struct *)work, clone_remote_thread);
	queue_work(popcorn_wq, (struct work_struct *)work);

	return 0;
}


///////////////////////////////////////////////////////////////////////////////
// Handle remote works at the origin
///////////////////////////////////////////////////////////////////////////////
int request_remote_work(pid_t pid, struct pcn_kmsg_message *req)
{
	struct task_struct *tsk;
	tsk = __get_task_struct(pid);
	if (!tsk) {
		printk(KERN_INFO"%s: invalid origin task %d for remote work %d\n",
				__func__, pid, req->header.type);
		pcn_kmsg_free_msg(req);
		return -ESRCH;
	}
	BUG_ON(tsk->remote_work);
	tsk->remote_work = req;
	complete(&tsk->remote_work_pended);

	put_task_struct(tsk);
	return 0;
}

static int __process_remote_works(void)
{
	bool run = true;
	while (run) {
		struct pcn_kmsg_message *req;
		long ret;
		ret = wait_for_completion_interruptible_timeout(
				&current->remote_work_pended, HZ);
		if (ret == 0) continue; /* timeout */
		if (ret == -ERESTARTSYS) break;

		req = (struct pcn_kmsg_message *)current->remote_work;
		current->remote_work = NULL;
		smp_wmb();

		switch (req->header.type) {
		case PCN_KMSG_TYPE_REMOTE_PAGE_REQUEST:
			WARN_ON_ONCE("Not implemented yet!");
			break;
		case PCN_KMSG_TYPE_VMA_OP_REQUEST:
			process_remote_vma_op((vma_op_request_t *)req);
			break;
		case PCN_KMSG_TYPE_REMOTE_VMA_REQUEST:
			process_remote_vma_request(req);
			break;
		case PCN_KMSG_TYPE_TASK_EXIT_REMOTE:
			process_remote_task_exit(req);
			run = false;
			break;
		case PCN_KMSG_TYPE_TASK_MIGRATE_BACK:
			bring_back_remote_thread(req);
			run = false;
			break;
		}
	}
	return 0;
}


/**
 * Send a message to <dst_nid> for migrating a task <task>.
 * This function will ask the remote node to create a thread to host the task.
 * It returns <0 in error case.
 */
static int __request_clone_remote(int dst_nid, struct task_struct *tsk, void __user *uregs)
{
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
	if (get_file_path(mm->exe_file, req->exe_path, sizeof(req->exe_path))) {
		printk("%s: cannot get path to exe binary\n", __func__);
		ret = -ESRCH;
		goto out;
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

	/* Signals and handlers
	req->remote_blocked = tsk->blocked;
	req->remote_real_blocked = tsk->real_blocked;
	req->remote_saved_sigmask = tsk->saved_sigmask;
	req->remote_pending = tsk->pending;
	req->sas_ss_sp = tsk->sas_ss_sp;
	req->sas_ss_size = tsk->sas_ss_size;
	memcpy(req->action, tsk->sighand->action, sizeof(req->action));
	*/

	/* Register sets from userspace */
	ret = copy_from_user(&req->arch.regsets, uregs,
			regset_size(get_popcorn_node_arch(dst_nid)));
	BUG_ON(ret != 0);

	save_thread_info(tsk, &req->arch);

	ret = pcn_kmsg_send(dst_nid, req, sizeof(*req));

out:
	kfree(req);
	mmput(mm);

	return ret;
}

int do_migration(struct task_struct *tsk, int dst_nid, void __user *uregs)
{
	int ret;
	struct remote_context *rc;

	might_sleep();

	/* Want to avoid allocate this structure in the spinlock-ed area */
	rc = __alloc_remote_context(my_nid, tsk->tgid, false);

	if (!cmpxchg(&tsk->mm->remote, 0, rc) == 0) {
		kfree(rc);
	} else {
		/*
		 * This process is becoming a distributed one if it was not yet.
		 * The first thread get migrated attaches the remote context to
		 * mm->remote, which indicates this process is distributed.
		 */

		/*
		 * Setting mm->remote to remote_context indicates
		 * this process is distributed
		 */
		rc->mm = get_task_mm(tsk);
		rc->remote_tgids[my_nid] = tsk->tgid;

		__lock_remote_contexts_out(dst_nid);
		list_add(&rc->list, &__remote_contexts_out());
		__unlock_remote_contexts_out(dst_nid);
	}
	tsk->remote = get_task_remote(tsk);
	tsk->at_remote = false;

	ret = __request_clone_remote(dst_nid, tsk, uregs);
	if (ret < 0) return ret;
	return __process_remote_works();
}


/**
 * Migrate the specified task <task> to node <dst_nid>
 * Currently, this function will put the specified task to sleep,
 * and push its info over to the remote node.
 * The remote node will then create a new thread and import that
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


DEFINE_KMSG_RW_HANDLER(remote_task_exit, remote_task_exit_t, origin_pid);
DEFINE_KMSG_RW_HANDLER(back_migration, back_migration_request_t, origin_pid);
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

	REGISTER_KMSG_HANDLER(PCN_KMSG_TYPE_TASK_EXIT_REMOTE, remote_task_exit);
	REGISTER_KMSG_HANDLER(PCN_KMSG_TYPE_TASK_EXIT_ORIGIN, origin_task_exit);

	return 0;
}
