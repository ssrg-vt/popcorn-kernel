/*
 * Copyright (C) 2017-2018
 *
 *  Ho-Ren (Jack) Chuang <horenc@vt.edu>
 *  Sang-Hoon Kim <sanghoon@vt.edu>
 */

#include <linux/kthread.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>

#include "../../kernel/popcorn/types.h"
#include "common.h"

#define MAX_THREADS 32
#define DEFAULT_PAYLOAD_SIZE_KB	4
#define DEFAULT_NR_ITERATIONS 1

enum TEST_REQUEST_FLAG {
	TEST_REQUEST_FLAG_REPLY = 0,
	TEST_REQUEST_FLAG_RDMA_WRITE = 1,
};

#define TEST_REQUEST_FIELDS \
	unsigned long flags; \
	unsigned long done; \
	char msg[PCN_KMSG_MAX_PAYLOAD_SIZE -  \
		sizeof(unsigned long) * 2 \
	];
DEFINE_PCN_KMSG(test_request_t, TEST_REQUEST_FIELDS);

#define TEST_RDMA_REQUEST_FIELDS \
	dma_addr_t rdma_addr; \
	u32 rdma_key; \
	size_t size; \
	unsigned long done; \
	unsigned long flags;
DEFINE_PCN_KMSG(test_rdma_request_t, TEST_RDMA_REQUEST_FIELDS);

#define TEST_RESPONSE_FIELDS \
	unsigned long done;
DEFINE_PCN_KMSG(test_response_t, TEST_RESPONSE_FIELDS);


enum test_action {
	TEST_ACTION_SEND = 0,
	TEST_ACTION_POST,
	TEST_ACTION_RDMA_WRITE,
	TEST_ACTION_RDMA_READ,
	TEST_ACTION_MAX,
};

struct test_params {
	int tid;
	enum test_action action;

	unsigned int nr_threads;
	unsigned long nr_iterations;
	size_t payload_size;

	struct test_barrier *barrier;
	void *private;
};


/**
 * Barrier to synchronize test threads
 */
struct test_barrier {
	spinlock_t lock;
	atomic_t count;
	unsigned int _count;
	wait_queue_head_t waiters;
};

static inline void __barrier_init(struct test_barrier *barrier, unsigned int nr)
{
	spin_lock_init(&barrier->lock);
	atomic_set(&barrier->count, nr);
	barrier->_count = nr;
	init_waitqueue_head(&barrier->waiters);
}

static inline void __barrier_wait(struct test_barrier *barrier)
{
	unsigned long flags;
	DEFINE_WAIT(wait);

	spin_lock_irqsave(&barrier->lock, flags);
	if (atomic_dec_and_test(&barrier->count)) {
		atomic_set(&barrier->count, barrier->_count);
		spin_unlock_irqrestore(&barrier->lock, flags);
		wake_up_all(&barrier->waiters);
	} else {
		prepare_to_wait(&barrier->waiters, &wait, TASK_INTERRUPTIBLE);
		spin_unlock_irqrestore(&barrier->lock, flags);

		schedule();
		finish_wait(&barrier->waiters, &wait);
	}
}

/**
 * Fundamental performance tests
 */
static int test_send(void *arg)
{
	struct test_params *param = arg;
	DECLARE_COMPLETION_ONSTACK(done);
	test_request_t *req;
	int i;
	char buffer[256];
	size_t msg_size = PCN_KMSG_SIZE(param->payload_size);

	__barrier_wait(param->barrier);
	for (i = 0; i < param->nr_iterations; i++) {
		if (msg_size > sizeof(buffer)) {
			req = kmalloc(sizeof(msg_size), GFP_KERNEL);
			BUG_ON(!req);
		} else {
			req = (void *)buffer;
		}

		req->flags = 0;
		set_bit(TEST_REQUEST_FLAG_REPLY, &req->flags);
		req->done = (unsigned long)&done;
		*(unsigned long *)req->msg = 0xcafe00dead00beef;

		pcn_kmsg_send(PCN_KMSG_TYPE_TEST_REQUEST, !my_nid, req, msg_size);

		wait_for_completion(&done);
		if (msg_size > sizeof(buffer)) {
			kfree(req);
		}
	}
	__barrier_wait(param->barrier);
	return 0;
}

static int test_post(void *arg)
{
	struct test_params *param = arg;
	DECLARE_COMPLETION_ONSTACK(done);
	test_request_t *req;
	int i;

	__barrier_wait(param->barrier);
	for (i = 0; i < param->nr_iterations; i++) {
		req = pcn_kmsg_get(PCN_KMSG_SIZE(param->payload_size));

		req->flags = 0;
		set_bit(TEST_REQUEST_FLAG_REPLY, &req->flags);
		req->done = (unsigned long)&done;
		*(unsigned long *)req->msg = 0xcafe00dead00beef;

		pcn_kmsg_post(PCN_KMSG_TYPE_TEST_REQUEST,
				!my_nid, req, PCN_KMSG_SIZE(param->payload_size));

		wait_for_completion(&done);
	}
	__barrier_wait(param->barrier);
	return 0;
}

static void process_test_send_request(struct work_struct *work)
{
	START_KMSG_WORK(test_request_t, req, work);
	if (test_bit(TEST_REQUEST_FLAG_REPLY, &req->flags)) {
		test_response_t *res = pcn_kmsg_get(sizeof(*res));
		res->done = req->done;

		pcn_kmsg_post(PCN_KMSG_TYPE_TEST_RESPONSE,
				PCN_KMSG_FROM_NID(req), res, sizeof(*res));
	}
	END_KMSG_WORK(req);
}

static int handle_test_send_response(struct pcn_kmsg_message *msg)
{
	test_response_t *res = (test_response_t *)msg;
	if (res->done) {
		complete((struct completion *)res->done);
	}

	pcn_kmsg_done(res);
	return 0;
}


/**
 * Test RDMA features
 */
static int test_rdma_write(void *arg)
{
	struct test_params *param = arg;
	int i;

	__barrier_wait(param->barrier);
	for (i = 0; i < param->nr_iterations; i++) {
		DECLARE_COMPLETION_ONSTACK(done);
		struct pcn_kmsg_rdma_handle *rh =
				pcn_kmsg_pin_rdma_buffer(NULL, PAGE_SIZE);
		test_rdma_request_t req = {
			.rdma_addr = rh->dma_addr,
			.rdma_key = rh->rkey,
			.size = param->payload_size,
			.done = (unsigned long)&done,
		};

		*(unsigned long *)rh->addr = 0xcafecaf00eadcafe;

		pcn_kmsg_send(PCN_KMSG_TYPE_TEST_RDMA_REQUEST,
				!my_nid, &req, sizeof(req));
		wait_for_completion(&done);

		pcn_kmsg_unpin_rdma_buffer(rh);
	}
	__barrier_wait(param->barrier);
	return 0;
}

static int test_rdma_read(void *arg)
{
	struct test_params *param = arg;

	__barrier_wait(param->barrier);
	__barrier_wait(param->barrier);
	return 0;
}

static void process_test_rdma_request(struct work_struct *work)
{
	START_KMSG_WORK(test_rdma_request_t, req, work);
	void *buffer = (void *)__get_free_page(GFP_KERNEL);
	test_response_t *res = kmalloc(sizeof(*res), GFP_KERNEL);
	int ret;

	if (req->flags & TEST_REQUEST_FLAG_RDMA_WRITE) {
		*(unsigned long *)buffer = 0xbaffdeafbeefface;

		ret = pcn_kmsg_rdma_write(PCN_KMSG_FROM_NID(req),
				req->rdma_addr, buffer, req->size, req->rdma_key);
	} else {
		ret = pcn_kmsg_rdma_read(PCN_KMSG_FROM_NID(req),
				buffer, req->rdma_addr, req->size, req->rdma_key);
	}
	res->done = req->done;
	pcn_kmsg_send(PCN_KMSG_TYPE_TEST_RESPONSE,
				PCN_KMSG_FROM_NID(req), res, sizeof(*res));

	free_page((unsigned long)buffer);
	END_KMSG_WORK(req);
}

/**
 * Run tests!
 */
struct test_desc {
	int (*test_fn)(void *);
	char *description;
};

static struct test_desc tests[] = {
	[TEST_ACTION_SEND]			= { test_send, "synchronous send"  },
	[TEST_ACTION_POST]			= { test_post, "synchronous post" },
	[TEST_ACTION_RDMA_WRITE]	= { test_rdma_write, "RDMA write" },
	[TEST_ACTION_RDMA_READ]		= { test_rdma_read, "RDMA read" },
};

static void __run_test(enum test_action action, struct test_params *param)
{
	struct test_params thread_params[MAX_THREADS] = {};
	struct task_struct *tsks[MAX_THREADS] = { NULL };
	struct test_barrier barrier;
	ktime_t t_start, t_end;
	DECLARE_COMPLETION_ONSTACK(done);
	unsigned long elapsed;
	int i;

	printk("Starting testing %s with %lu payload, %u thread%s, %lu iteration%s\n",
			tests[action].description, param->payload_size,
			param->nr_threads, param->nr_threads == 1 ? "" : "s",
			param->nr_iterations, param->nr_iterations == 1 ? "" : "s");

	__barrier_init(&barrier, param->nr_threads + 1);
	param->barrier = &barrier;

	for (i = 0; i < param->nr_threads; i++) {
		struct test_params *thr_param = thread_params + i;

		*thr_param = *param;
		thr_param->tid = i;

		tsks[i] = kthread_run(tests[action].test_fn, thr_param, "test_%d", i);
	}

	__barrier_wait(&barrier);
	t_start = ktime_get();
	/* run the test */
	__barrier_wait(&barrier);
	t_end = ktime_get();


	elapsed = ktime_to_ns(ktime_sub(t_end, t_start));

	printk("Done testing %s\n", tests[action].description);
	printk("  %9lu ns in total\n", elapsed);
	printk("  %3lu.%05lu ns per operation\n",
			elapsed / param->nr_iterations,
			(elapsed % param->nr_iterations) * 1000 /  param->nr_iterations);
}


static int __parse_cmd(const char __user *buffer, size_t count, struct test_params *params)
{
	int args;
	int action;
	char *cmd;
	unsigned long payload_size, iter, nr_threads;

	cmd = kmalloc(count, GFP_KERNEL);
	if (!cmd) {
		printk(KERN_ERR "kmalloc failure\n");
		return -ENOMEM;
	}
	if (copy_from_user(cmd, buffer, count)) {
		kfree(cmd);
		return -EFAULT;
	}

	args = sscanf(cmd, "%d %lu %lu %lu",
			&action, &payload_size, &nr_threads, &iter);
	if (args <= 0) {
		printk(KERN_ERR "Wrong command\n");
		kfree(cmd);
		return -EINVAL;
	}

	params->action = action;

	if (args >= 2) {
		if (payload_size < sizeof(unsigned long) * 2) {
			printk(KERN_ERR "Payload should be larger than %ld\n",
					sizeof(unsigned long) * 2);
			kfree(cmd);
			return -EINVAL;
		}
		if (payload_size > PCN_KMSG_MAX_SIZE) {
			printk(KERN_ERR "Payload should be less than %lu KB\n",
					PCN_KMSG_MAX_SIZE >> 10);
			kfree(cmd);
			return -EINVAL;
		}
		params->payload_size = payload_size;
	}
	if (args >= 3) {
		if (nr_threads > MAX_THREADS) {
			printk(KERN_ERR "# of threads cannot be larger than %d\n",
					MAX_THREADS);
			kfree(cmd);
			return -EINVAL;
		}
		params->nr_threads = nr_threads;
	}
	if (args >= 4)
		params->nr_iterations = iter;

	kfree(cmd);
	return 0;
}

static ssize_t start_test(struct file *file, const char __user *buffer, size_t count, loff_t *ppos)
{
	int ret;
	int action;

	struct test_params params = {
		.payload_size = DEFAULT_PAYLOAD_SIZE_KB << 10,
		.nr_threads = 1,
		.nr_iterations = DEFAULT_NR_ITERATIONS,
	};

	if ((ret = __parse_cmd(buffer, count, &params))) {
		return ret;
	}
	action = params.action;

	if (!try_module_get(THIS_MODULE))
		return -EPERM;

	/* do the coresponding work */
	switch(action) {
	case TEST_ACTION_SEND:
	case TEST_ACTION_POST:
		__run_test(action, &params);
		break;
	case TEST_ACTION_RDMA_WRITE:
	case TEST_ACTION_RDMA_READ:
		if (pcn_kmsg_has_features(PCN_KMSG_FEATURE_RDMA)) {
			__run_test(action, &params);
		} else {
			printk(KERN_ERR "Transport does not support RDMA.\n");
		}
		break;
	default:
		printk("Unknown test no #%d\n", action);
	}

	module_put(THIS_MODULE);
	return count;
}

static void __show_usage(void)
{
	int i;
	printk(" Usage: echo [action] {payload size in byte} {# of threads} \\\n");
	printk("                      {# of iterations} > /proc/msg_test\n");
	printk(" Default: %d KB payload, iterate %d time%s, single thread\n",
			DEFAULT_PAYLOAD_SIZE_KB,
			DEFAULT_NR_ITERATIONS, DEFAULT_NR_ITERATIONS == 1 ? "" : "s");

	printk(" Tests:\n");
	for (i = 0; i < TEST_ACTION_MAX; i++) {
		if (!tests[i].test_fn) continue;
		printk("  %d: %s\n", i, tests[i].description);
	}
	printk("\n");
}

static int kmsg_test_read_proc(struct seq_file *seq, void *v)
{
	return 0;
}

static int kmsg_test_read_open(struct inode *inode, struct file *file)
{
	return single_open(file, kmsg_test_read_proc, inode->i_private);
}

static struct file_operations kmsg_test_ops = {
	.owner = THIS_MODULE,
	.open = kmsg_test_read_open,
	.llseek  = seq_lseek,
	.release = single_release,
	.write = start_test,
};


DEFINE_KMSG_WQ_HANDLER(test_send_request);
DEFINE_KMSG_WQ_HANDLER(test_rdma_request);

static struct proc_dir_entry *kmsg_test_proc = NULL;

static int __init msg_test_init(void)
{
	printk("\nLoading Popcorn messaging layer tester...\n");

#ifdef CONFIG_POPCORN_STAT
	printk(KERN_WARNING " * You are collecting statistics "
			"and may get inaccurate performance data now *\n");
#endif

	/* register a proc fs entry */
	kmsg_test_proc = proc_create("msg_test", 0666, NULL, &kmsg_test_ops);
	if (!kmsg_test_proc) {
		printk(KERN_ERR " Cannot create /proc/msg_test\n");
		return -EPERM;
	}

	REGISTER_KMSG_WQ_HANDLER(PCN_KMSG_TYPE_TEST_REQUEST, test_send_request);
	REGISTER_KMSG_HANDLER(PCN_KMSG_TYPE_TEST_RESPONSE, test_send_response);
	REGISTER_KMSG_WQ_HANDLER(PCN_KMSG_TYPE_TEST_RDMA_REQUEST, test_rdma_request);

	__show_usage();
	return 0;
}

static void __exit msg_test_exit(void)
{
	if (kmsg_test_proc) proc_remove(kmsg_test_proc);

	printk("Unloaded Popcorn messaging layer tester. Good bye!\n");
}

module_init(msg_test_init);
module_exit(msg_test_exit);
MODULE_LICENSE("GPL");
