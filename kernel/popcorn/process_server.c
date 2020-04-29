// SPDX-License-Identifier: GPL-2.0, BSD
/*
 * /kernel/popcorn/process_server.c
 *
 * Popcorn Linux thread migration implementation.
 *
 * This is Popcorn's core migration handler. It
 * also defines and registers handlers for
 * other key Popcorn work.
 *
 * This work was an extension of David Katz MS Thesis,
 * rewritten by Sang-Hoon to support multithread
 * environment.
 *
 * author, Javier Malave, Rebecca Shapiro, Andrew Hughes,
 * Narf Industries 2020 (modifications for upstream RFC)
 * author Sang-Hoon Kim, SSRG Virginia Tech 2017
 * author Antonio Barbalace, SSRG Virginia Tech 2014-2016
 * author Vincent Legout, Sharat Kumar Bath, Ajithchandra Saya, SSRG Virginia Tech 2014-2015
 * author David Katz, Marina Sadini, SSRG Virginia 2013
 */

#include <linux/sched.h>
#include <linux/threads.h>
#include <linux/slab.h>
#include <linux/kthread.h>
#include <linux/ptrace.h>
#include <linux/mmu_context.h>
#include <linux/fs.h>
#include <linux/futex.h>
#include <linux/sched/mm.h>
#include <linux/uaccess.h>

#include <asm/mmu_context.h>
#include <asm/kdebug.h>

#include <popcorn/bundle.h>

#include "types.h"
#include "process_server.h"
#include "vma_server.h"
#include "page_server.h"
#include "wait_station.h"
#include "util.h"

static struct list_head remote_contexts[2];
static spinlock_t remote_contexts_lock[2];

inline void __lock_remote_contexts(spinlock_t *remote_contexts_lock, int index)
{
	spin_lock(remote_contexts_lock + index);
}

inline void __unlock_remote_contexts(spinlock_t *remote_contexts_lock, int index)
{
	spin_unlock(remote_contexts_lock + index);
}

/* Hold the corresponding remote_contexts_lock */
static struct remote_context *__lookup_remote_contexts_in(int nid, int tgid)
{
	struct remote_context *rc;

	list_for_each_entry(rc, remote_contexts + INDEX_INBOUND, list) {
		if (rc->remote_tgids[nid] == tgid)
			return rc;
	}
	return NULL;
}

inline struct remote_context *__get_mm_remote(struct mm_struct *mm)
{
	struct remote_context *rc = mm->remote;
	atomic_inc(&rc->count);
	return rc;
}

inline struct remote_context *get_task_remote(struct task_struct *tsk)
{
	return __get_mm_remote(tsk->mm);
}

inline bool __put_task_remote(struct remote_context *rc)
{
	if (!atomic_dec_and_test(&rc->count)) return false;

	__lock_remote_contexts(remote_contexts_lock, rc->for_remote);
#ifdef CONFIG_POPCORN_CHECK_SANITY
	BUG_ON(atomic_read(&rc->count));
#endif
	list_del(&rc->list);
	__unlock_remote_contexts(remote_contexts_lock, rc->for_remote);

	free_remote_context_pages(rc);
	kfree(rc);
	return true;
}

inline bool put_task_remote(struct task_struct *tsk)
{
	return __put_task_remote(tsk->mm->remote);
}

void free_remote_context(struct remote_context *rc)
{
#ifdef CONFIG_POPCORN_CHECK_SANITY
	BUG_ON(atomic_read(&rc->count) != 1 && atomic_read(&rc->count) != 2);
#endif
	__put_task_remote(rc);
}

static struct remote_context *__alloc_remote_context(int nid, int tgid,
						     bool remote)
{
	struct remote_context *rc = kmalloc(sizeof(*rc), GFP_KERNEL);
	int i;

	if (!rc)
		return ERR_PTR(-ENOMEM);

	INIT_LIST_HEAD(&rc->list);
	atomic_set(&rc->count, 1); /* Account for mm->remote in a near future */
	rc->mm = NULL;

	rc->tgid = tgid;
	rc->for_remote = remote;

	for (i = 0; i < FAULTS_HASH; i++) {
		INIT_HLIST_HEAD(&rc->faults[i]);
		spin_lock_init(&rc->faults_lock[i]);
	}

	INIT_LIST_HEAD(&rc->vmas);
	spin_lock_init(&rc->vmas_lock);

	rc->stop_remote_worker = false;

	rc->remote_worker = NULL;
	INIT_LIST_HEAD(&rc->remote_works);
	spin_lock_init(&rc->remote_works_lock);
	init_completion(&rc->remote_works_ready);

	memset(rc->remote_tgids, 0x00, sizeof(rc->remote_tgids));

	INIT_RADIX_TREE(&rc->pages, GFP_ATOMIC);

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

/*
 *  This function implements a distributed mutex.
 */
long process_server_do_futex_at_remote(u32 __user *uaddr, int op, u32 val,
				       bool valid_ts, struct timespec64 *ts,
				       u32 __user *uaddr2, u32 val2, u32 val3)
{
	struct wait_station *ws = get_wait_station(current);
	remote_futex_request req = {
		.origin_pid = current->origin_pid,
		.remote_ws = ws->id,
		.op = op,
		.val = val,
		.ts = {
			.tv_sec = -1,
		},
		.uaddr = uaddr,
		.uaddr2 = uaddr2,
		.val2 = val2,
		.val3 = val3,
	};
	remote_futex_response *res;
	long ret = 0;

	if (valid_ts) {
		req.ts = *ts;
	}


	pcn_kmsg_send(PCN_KMSG_TYPE_FUTEX_REQUEST,
			current->origin_nid, &req, sizeof(req));

	res = wait_at_station(ws);
	ret = res->ret;
	pcn_kmsg_done(res);

	return ret;
}

static int handle_remote_futex_response(struct pcn_kmsg_message *msg)
{
	remote_futex_response *res = (remote_futex_response *)msg;
	struct wait_station *ws = wait_station(res->remote_ws);

	ws->private = res;
	complete(&ws->pendings);
	return 0;
}

static int process_remote_futex_request(remote_futex_request *req)
{
	int ret;
	int err = 0;
	remote_futex_response *res;
	ktime_t t, *tp = NULL;

	if (timespec64_valid(&req->ts)) {
		t = timespec64_to_ktime(req->ts);
		t = ktime_add_safe(ktime_get(), t);
		tp = &t;
	}


	ret = do_futex(req->uaddr, req->op, req->val,
			tp, req->uaddr2, req->val2, req->val3);

	res = pcn_kmsg_get(sizeof(*res));

	if(!res) {
		err = -ENOMEM;
		goto out;
	}

	res->remote_ws = req->remote_ws;
	res->ret = ret;

	pcn_kmsg_post(PCN_KMSG_TYPE_FUTEX_RESPONSE,
			current->remote_nid, res, sizeof(*res));

out:
	pcn_kmsg_done(req);
	return err;
}

/*
 *  This function handles process exit.
 */
static void __terminate_remotes(struct remote_context *rc)
{
	int nid;
	origin_task_exit_t req = {
		.origin_pid = current->pid,
		.exit_code = current->exit_code,
	};

	/* Take down peer vma workers */
	for (nid = 0; nid < MAX_POPCORN_NODES; nid++) {
		if (nid == my_nid || rc->remote_tgids[nid] == 0) continue;
		PSPRINTK("TERMINATE [%d/%d] with 0x%d\n",
				rc->remote_tgids[nid], nid, req.exit_code);

		req.remote_pid = rc->remote_tgids[nid];
		pcn_kmsg_send(PCN_KMSG_TYPE_TASK_EXIT_ORIGIN, nid, &req, sizeof(req));
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
	 */
	if (atomic_read(&tsk->mm->mm_users) == 1) {
		__terminate_remotes(rc);
	}

	return 0;
}

static int __exit_remote_task(struct task_struct *tsk)
{
	/* Something went south. Notify the origin. */
	if (tsk->exit_code != TASK_PARKED) {
		if (!get_task_remote(tsk)->stop_remote_worker) {
			remote_task_exit_t req = {
				.origin_pid = tsk->origin_pid,
				.remote_pid = tsk->pid,
				.exit_code = tsk->exit_code,
			};
			pcn_kmsg_send(PCN_KMSG_TYPE_TASK_EXIT_REMOTE,
					tsk->origin_nid, &req, sizeof(req));
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

	if (!distributed_process(tsk))
		return -ESRCH;

	PSPRINTK("EXITED [%d] %s%s / 0x%x\n", tsk->pid,
			tsk->at_remote ? "remote" : "local",
			tsk->is_worker ? " worker": "",
			tsk->exit_code);

	if (tsk->is_worker)
		return 0;

	if (tsk->at_remote)
		return __exit_remote_task(tsk);
	else
		return __exit_origin_task(tsk);
}

/*
 * Handle the notification of the task kill at the remote.
 */
static void process_remote_task_exit(remote_task_exit_t *req)
{
	struct task_struct *tsk = current;
	int exit_code = req->exit_code;

	if (tsk->remote_pid != req->remote_pid) {
		printk(KERN_INFO"%s: pid mismatch %d != %d\n", __func__,
				tsk->remote_pid, req->remote_pid);
		pcn_kmsg_done(req);
		return;
	}

	PSPRINTK("%s [%d] 0x%x\n", __func__, tsk->pid, req->exit_code);

	tsk->remote = NULL;
	tsk->remote_nid = -1;
	tsk->remote_pid = -1;
	put_task_remote(tsk);

	exit_code = req->exit_code;
	pcn_kmsg_done(req);

	if (exit_code & CSIGNAL)
		force_sig(exit_code & CSIGNAL, tsk);

	do_exit(exit_code);
}

static void process_origin_task_exit(struct remote_context *rc,
				     origin_task_exit_t *req)
{
	BUG_ON(!current->is_worker);

	PSPRINTK("\nTERMINATE [%d] with 0x%x\n", current->pid, req->exit_code);
	current->exit_code = req->exit_code;
	rc->stop_remote_worker = true;

	pcn_kmsg_done(req);
}

/*
 *    This function handles back migration.
 *    If there is a pid mismatch the function will exit.
 *    Otherwise it will restore the process at origin.
 */
static void process_back_migration(back_migration_request_t *req)
{
	if (current->remote_pid != req->remote_pid) {
		printk(KERN_INFO"%s: pid mismatch during back migration (%d != %d)\n",
				__func__, current->remote_pid, req->remote_pid);
		goto out_free;
	}

	PSPRINTK("### BACKMIG [%d] from [%d/%d]\n",
			current->pid, req->remote_pid, req->remote_nid);

	/* Welcome home */

	current->remote = NULL;
	current->remote_nid = -1;
	current->remote_pid = -1;
	put_task_remote(current);

	current->personality = req->personality;

	/* XXX signals */

	/* mm is not updated here; has been synchronized through vma operations */

	restore_thread_info(&req->arch, true);

out_free:
	pcn_kmsg_done(req);
}

/*
 * Send a message to <dst_nid> for migrating back a task <task>.
 * This is a back migration
 *  => <task> must already been migrated to <dst_nid>.
 * It returns -1 in error case.
 */
static int __do_back_migration(struct task_struct *tsk, int dst_nid,
			       void __user *uregs)
{
	back_migration_request_t *req;
	int ret = 0;
	int size = 0;

	might_sleep();

	BUG_ON(tsk->origin_nid == -1 && tsk->origin_pid == -1);

	req = pcn_kmsg_get(sizeof(*req));

	if(!req) {
		return -ENOMEM;
	}

	req->origin_pid = tsk->origin_pid;
	req->remote_nid = my_nid;
	req->remote_pid = tsk->pid;

	req->personality = tsk->personality;

	size = regset_size(get_popcorn_node_arch(dst_nid));
	if(!size) {
		return -EINVAL;

	}
	ret = copy_from_user(&req->arch.regsets, uregs,
			size);

	save_thread_info(&req->arch);

	ret = pcn_kmsg_post(
		PCN_KMSG_TYPE_TASK_MIGRATE_BACK, dst_nid, req, sizeof(*req));

	do_exit(TASK_PARKED);

	return ret;
}

/*
 *  Remote thread
 */
static int handle_remote_task_pairing(struct pcn_kmsg_message *msg)
{
	remote_task_pairing_t *req = (remote_task_pairing_t *)msg;
	struct task_struct *tsk;
	int from_nid = PCN_KMSG_FROM_NID(req);
	int ret = 0;

	tsk = __get_task_struct(req->your_pid);
	if (!tsk) {
		ret = -ESRCH;
		goto out;
	}
	BUG_ON(tsk->at_remote);
	BUG_ON(!tsk->remote);

	tsk->remote_nid = from_nid;
	tsk->remote_pid = req->my_pid;
	tsk->remote->remote_tgids[from_nid] = req->my_tgid;

	put_task_struct(tsk);
out:
	pcn_kmsg_done(req);
	return ret;
}

static int __pair_remote_task(void)
{
	remote_task_pairing_t req = {
		.my_tgid = current->tgid,
		.my_pid = current->pid,
		.your_pid = current->origin_pid,
	};
	return pcn_kmsg_send(
			PCN_KMSG_TYPE_TASK_PAIRING, current->origin_nid, &req, sizeof(req));
}


struct remote_thread_params {
	clone_request_t *req;
};

/*
 *   This function performs the main proceess
 *   migration operation. It demotes temporary priviledge prior
 *   to injecting the thread info into the clone request.
 *   Upon success, the function returns to user-space.
 */
static int remote_thread_main(void *_args)
{
	struct remote_thread_params *params = _args;
	clone_request_t *req = params->req;
	int ret = 0;

#ifdef CONFIG_POPCORN_DEBUG_VERBOSE
	PSPRINTK("%s [%d] started for [%d/%d]\n", __func__,
			current->pid, req->origin_pid, PCN_KMSG_FROM_NID(req));
#endif

	current->flags &= ~PF_KTHREAD;
	current->origin_nid = PCN_KMSG_FROM_NID(req);
	current->origin_pid = req->origin_pid;
	current->remote = get_task_remote(current);

	set_fs(USER_DS);

	restore_thread_info(&req->arch, true);

	if((ret = __pair_remote_task())) {
#ifdef CONFIG_POPCORN_DEBUG_VERBOSE
		PSPRINTK("%s [%d] failed __pair_remote_task() for [%d/%d]\n", __func__,
			current->pid, req->origin_pid, PCN_KMSG_FROM_NID(req));
#endif
		return ret;
	}

	PSPRINTK("\n####### MIGRATED - [%d/%d] from [%d/%d]\n",
			current->pid, my_nid, current->origin_pid, current->origin_nid);

	kfree(params);
	pcn_kmsg_done(req);

	return ret;
}

static int __fork_remote_thread(clone_request_t *req)
{
	struct remote_thread_params *params;
	params = kmalloc(sizeof(*params), GFP_KERNEL);
	params->req = req;

	/* The loop deals with signals between concurrent migration */
	while (kernel_thread(remote_thread_main, params,
					CLONE_THREAD | CLONE_SIGHAND | SIGCHLD) < 0) {
		schedule();
	}
	return 0;
}

static int __construct_mm(clone_request_t *req, struct remote_context *rc)
{
	struct mm_struct *mm;
	struct file *f;
	struct rlimit rlim_stack;

	if(!(mm = mm_alloc())) {
		return -ENOMEM;
	}

	task_lock(current->group_leader);
	rlim_stack = current->signal->rlim[RLIMIT_STACK];
	task_unlock(current->group_leader);

	arch_pick_mmap_layout(mm, &rlim_stack);

	f = filp_open(req->exe_path, O_RDONLY | O_LARGEFILE | O_EXCL, 0);
	if (IS_ERR(f)) {
		PCNPRINTK_ERR("cannot open executable from %s\n", req->exe_path);
		mmdrop(mm);
		return -EINVAL;
	}
	set_mm_exe_file(mm, f);
	filp_close(f, NULL);

	mm->task_size = req->task_size;
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

static void __terminate_remote_threads(struct remote_context *rc)
{
	struct task_struct *tsk;

	/* Terminate userspace threads. Tried to use do_group_exit() but it
	 * didn't work */
	rcu_read_lock();
	for_each_thread(current, tsk) {
		if (tsk->is_worker)
			continue;
		force_sig(current->exit_code, tsk);
	}
	rcu_read_unlock();
}

static void __run_remote_worker(struct remote_context *rc)
{
	while (!rc->stop_remote_worker) {
		struct work_struct *work = NULL;
		struct pcn_kmsg_message *msg;
		int wait_ret;
		unsigned long flags;

		wait_ret = wait_for_completion_interruptible_timeout(
					&rc->remote_works_ready, HZ);
		if (wait_ret == 0)
			continue;

		spin_lock_irqsave(&rc->remote_works_lock, flags);

		if (!list_empty(&rc->remote_works)) {
			work = list_first_entry(
					&rc->remote_works, struct work_struct, entry);
			list_del(&work->entry);
		}
		spin_unlock_irqrestore(&rc->remote_works_lock, flags);

		if (!work)
			continue;

		msg = ((struct pcn_kmsg_work *)work)->msg;

		switch (msg->header.type) {
		case PCN_KMSG_TYPE_TASK_MIGRATE:
			__fork_remote_thread((clone_request_t *)msg);
			break;
		case PCN_KMSG_TYPE_VMA_OP_REQUEST:
			process_vma_op_request((vma_op_request_t *)msg);
			break;
		case PCN_KMSG_TYPE_TASK_EXIT_ORIGIN:
			process_origin_task_exit(rc, (origin_task_exit_t *)msg);
			break;
		default:
			printk("Unknown remote work type %d\n", msg->header.type);
			break;
		}

		/* msg is released (pcn_kmsg_done()) in each handler */
		kfree(work);
	}
}

struct remote_worker_params {
	clone_request_t *req;
	struct remote_context *rc;
	char comm[TASK_COMM_LEN];
};

static int remote_worker_main(void *data)
{
	struct remote_worker_params *params = (struct remote_worker_params *)data;
	struct remote_context *rc = params->rc;
	clone_request_t *req = params->req;
	int mm_err = 0;

	might_sleep();
	kfree(params);

	PSPRINTK("%s: [%d] for [%d/%d]\n", __func__,
			current->pid, req->origin_tgid, PCN_KMSG_FROM_NID(req));
	PSPRINTK("%s: [%d] %s\n", __func__,
			current->pid, req->exe_path);

	current->flags &= ~PF_RANDOMIZE;	/* Disable ASLR for now*/
	current->flags &= ~PF_KTHREAD;	/* Demote to a user thread */

	current->personality = req->personality;
	current->is_worker = true;
	current->at_remote = true;
	current->origin_nid = PCN_KMSG_FROM_NID(req);
	current->origin_pid = req->origin_pid;

	set_user_nice(current, 0);

	if ((mm_err = __construct_mm(req, rc))) {
		return mm_err;
	}

	get_task_remote(current);
	rc->tgid = current->tgid;

	__run_remote_worker(rc);

	__terminate_remote_threads(rc);

	put_task_remote(current);
	return current->exit_code;
}

static void __schedule_remote_work(struct remote_context *rc,
				   struct pcn_kmsg_work *work)
{
	/* Exploit the list_head in work_struct */
	struct list_head *entry = &((struct work_struct *)work)->entry;
	unsigned long flags;

	INIT_LIST_HEAD(entry);
	spin_lock_irqsave(&rc->remote_works_lock, flags);
	list_add(entry, &rc->remote_works);
	spin_unlock_irqrestore(&rc->remote_works_lock, flags);

	complete(&rc->remote_works_ready);
}

static void clone_remote_thread(struct work_struct *_work)
{
	struct pcn_kmsg_work *work = (struct pcn_kmsg_work *)_work;
	clone_request_t *req = work->msg;
	int nid_from = PCN_KMSG_FROM_NID(req);
	int tgid_from = req->origin_tgid;
	struct remote_context *rc;
	struct remote_context *rc_new =
			__alloc_remote_context(nid_from, tgid_from, true);

	BUG_ON(!rc_new);

	__lock_remote_contexts(remote_contexts_lock, INDEX_INBOUND);
	rc = __lookup_remote_contexts_in(nid_from, tgid_from);
	if (!rc) {
		struct remote_worker_params *params;

		rc = rc_new;
		rc->remote_tgids[nid_from] = tgid_from;
		list_add(&rc->list, remote_contexts + INDEX_INBOUND);
		__unlock_remote_contexts(remote_contexts_lock, INDEX_INBOUND);

		params = kmalloc(sizeof(*params), GFP_KERNEL);
		BUG_ON(!params);

		params->rc = rc;
		params->req = req;
		__build_task_comm(params->comm, req->exe_path);
		smp_wmb();

		rc->remote_worker =
				kthread_run(remote_worker_main, params, params->comm);
	} else {
		__unlock_remote_contexts(remote_contexts_lock, INDEX_INBOUND);
		kfree(rc_new);
	}

	/* Schedule this fork request */
	__schedule_remote_work(rc, work);
	return;
}

static int handle_clone_request(struct pcn_kmsg_message *msg)
{
	clone_request_t *req = (clone_request_t *)msg;
	struct pcn_kmsg_work *work = kmalloc(sizeof(*work), GFP_ATOMIC);
	if(!work)
		return -ENOMEM;

	work->msg = req;
	INIT_WORK((struct work_struct *)work, clone_remote_thread);
	queue_work(popcorn_wq, (struct work_struct *)work);

	return 0;
}

/*
 * Handle remote works at the origin
 */
int request_remote_work(pid_t pid, struct pcn_kmsg_message *req)
{
	struct task_struct *tsk = __get_task_struct(pid);

	if (!tsk) {
		printk(KERN_INFO"%s: invalid origin task %d for remote work %d\n",
				__func__, pid, req->header.type);
		pcn_kmsg_done(req);
		return -ESRCH;
	}

	/*
	 * Origin-initiated remote works are node-wide operations, thus, enqueue
	 * such requests into the remote work queue.
	 * On the other hand, remote-initated remote works are thread-wise requests.
	 * So, pending the requests to the per-thread work queue.
	 */
	if (tsk->at_remote) {
		struct remote_context *rc = get_task_remote(tsk);
		struct pcn_kmsg_work *work = kmalloc(sizeof(*work), GFP_ATOMIC);

		BUG_ON(!tsk->is_worker);
		work->msg = req;

		__schedule_remote_work(rc, work);

		__put_task_remote(rc);
	} else {
		BUG_ON(tsk->remote_work);
		tsk->remote_work = req;
		complete(&tsk->remote_work_pended);
	}

	put_task_struct(tsk);
	return 0;
}

static int __process_remote_works(void)
{
	int err = 0;
	bool run = true;
	BUG_ON(current->at_remote);

	while (run) {
		struct pcn_kmsg_message *req;
		long ret;
		ret = wait_for_completion_interruptible_timeout(
				&current->remote_work_pended, HZ);
		if (ret == 0)
			continue;

		req = (struct pcn_kmsg_message *)current->remote_work;
		current->remote_work = NULL;
		smp_wmb();

		if (!req)
			continue;

		switch (req->header.type) {
		case PCN_KMSG_TYPE_VMA_OP_REQUEST:
			process_vma_op_request((vma_op_request_t *)req);
			break;
		case PCN_KMSG_TYPE_VMA_INFO_REQUEST:
			process_vma_info_request((vma_info_request_t *)req);
			break;
		case PCN_KMSG_TYPE_FUTEX_REQUEST:
			err = process_remote_futex_request((remote_futex_request *)req);
			break;
		case PCN_KMSG_TYPE_TASK_EXIT_REMOTE:
			process_remote_task_exit((remote_task_exit_t *)req);
			run = false;
			break;
		case PCN_KMSG_TYPE_TASK_MIGRATE_BACK:
			process_back_migration((back_migration_request_t *)req);
			run = false;
			break;
		default:
			if (WARN_ON("Received unsupported remote work")) {
				printk("  type: %d\n", req->header.type);
			}
		}
	}
	return err;
}

/*
 * Send a message to <dst_nid> for migrating a task <task>.
 * This function will ask the remote node to create a thread to host the task.
 * It returns <0 in error case.
 */
static int __request_clone_remote(int dst_nid, struct task_struct *tsk,
				  void __user *uregs)
{
	struct mm_struct *mm = get_task_mm(tsk);
	clone_request_t *req;
	int ret = 0;
	int size = 0;

	req = pcn_kmsg_get(sizeof(*req));

	if (!req) {
		ret = -ENOMEM;
		goto out;
	}

	/* struct mm_struct */
	if (get_file_path(mm->exe_file, req->exe_path, sizeof(req->exe_path))) {
		printk("%s: cannot get path to exe binary\n", __func__);
		ret = -ESRCH;
		pcn_kmsg_put(req);
		goto out;
	}

	req->task_size = mm->task_size;
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
	req->origin_tgid = tsk->tgid;
	req->origin_pid = tsk->pid;

	req->personality = tsk->personality;

	/* Register sets from userspace */
	size = regset_size(get_popcorn_node_arch(dst_nid));
	if(!size) {
		return -EINVAL;

	}
	ret = copy_from_user(&req->arch.regsets, uregs,
			     size);

	save_thread_info(&req->arch);

	ret = pcn_kmsg_post(PCN_KMSG_TYPE_TASK_MIGRATE, dst_nid, req, sizeof(*req));

out:
	mmput(mm);
	return ret;
}

/*
 * do_migration takes care of the original process migration
 * to a remote node. Its complementary function being
 * do_back_migration which returns a process from a remote node
 * back to the origin node.
 *
 * The first thread gets migrated attaches the remote context to
 * mm->remote, which indicates some threads in this process is
 * distributed. At this point tsk->remote gets a link to the
 * remote context.
 *
 */
static int __do_migration(struct task_struct *tsk, int dst_nid,
			  void __user *uregs)
{
	int ret = 0;
	struct remote_context *rc;

	rc = __alloc_remote_context(my_nid, tsk->tgid, false);
	if (IS_ERR(rc))
		return PTR_ERR(rc);

	if (cmpxchg(&tsk->mm->remote, 0, rc)) {
		kfree(rc);
	} else {
		rc->mm = tsk->mm;
		rc->remote_tgids[my_nid] = tsk->tgid;

		__lock_remote_contexts(remote_contexts_lock, INDEX_OUTBOUND);
		list_add(&rc->list, remote_contexts + INDEX_OUTBOUND);
		__unlock_remote_contexts(remote_contexts_lock, INDEX_OUTBOUND);
	}

	tsk->remote = get_task_remote(tsk);

	ret = __request_clone_remote(dst_nid, tsk, uregs);
	if (ret)
		return ret;

	ret = __process_remote_works();
	return ret;
}

/*
 * Migrate the specified task <task> to node <dst_nid>
 * Currently, this function will put the specified task to sleep,
 * and push its info over to the remote node.
 * The remote node will then create a new thread and import that
 * info into its new context.
 */
int process_server_do_migration(struct task_struct *tsk, unsigned int dst_nid,
				void __user *uregs)
{
	int ret = 0;

	if (tsk->origin_nid == dst_nid) {
		ret = __do_back_migration(tsk, dst_nid, uregs);
	} else {
		ret = __do_migration(tsk, dst_nid, uregs);
		if (ret) {
			tsk->remote = NULL;
			tsk->remote_pid = tsk->remote_nid = -1;
			put_task_remote(tsk);
		}
	}

	return ret;
}

DEFINE_KMSG_RW_HANDLER(origin_task_exit, origin_task_exit_t, remote_pid);
DEFINE_KMSG_RW_HANDLER(remote_task_exit, remote_task_exit_t, origin_pid);
DEFINE_KMSG_RW_HANDLER(back_migration, back_migration_request_t, origin_pid);
DEFINE_KMSG_RW_HANDLER(remote_futex_request, remote_futex_request, origin_pid);

/*
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

	REGISTER_KMSG_HANDLER(PCN_KMSG_TYPE_FUTEX_REQUEST, remote_futex_request);
	REGISTER_KMSG_HANDLER(PCN_KMSG_TYPE_FUTEX_RESPONSE, remote_futex_response);

	return 0;
}
