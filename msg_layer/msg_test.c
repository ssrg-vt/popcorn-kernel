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

#define TEST_RDMA_DSMRR_REQUEST_FIELDS \
	dma_addr_t rdma_addr; \
	u32 rdma_key; \
	size_t size; \
	unsigned long done; \
	unsigned long flags;
DEFINE_PCN_KMSG(test_dsmrr_request_t, TEST_RDMA_DSMRR_REQUEST_FIELDS);

#define TEST_RESPONSE_FIELDS \
	unsigned long done;
DEFINE_PCN_KMSG(test_response_t, TEST_RESPONSE_FIELDS);


enum test_action {
	TEST_ACTION_SEND = 0,
	TEST_ACTION_POST,
	TEST_ACTION_RDMA_WRITE,
	TEST_ACTION_RDMA_READ,
	TEST_ACTION_DSM_RR,
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


#if 0
/* for mimicing RW */
char *dummy_send_buf[MAX_NUM_NODES][MAX_THREADS];

/* For testing RDMA READ/WRITE */
char *dummy_act_buf[MAX_NUM_NODES][MAX_THREADS];
char *dummy_pass_buf[MAX_NUM_NODES][MAX_THREADS];

/* Buffers for testing RDMA RW */
int g_remote_read_len = 8 * 1024;
int g_rdma_write_len = 8 * 1024;
char *g_test_buf = NULL;
char *g_test_write_buf = NULL;

#define RDMA_TEST_FIELDS \
	int remote_ws; \
	u64 dma_addr_act; \
	u32 mr_id; \
	int t_num;
DEFINE_PCN_RDMA_KMSG(pcn_kmsg_perf_rdma_t, RDMA_TEST_FIELDS);

struct mimic_rw_msg_request {
	struct pcn_kmsg_hdr header;
	int tid;
	int remote_ws;
	unsigned int size;
	unsigned char payload[MAX_MSG_LENGTH];
};

struct mimic_rw_signal_request {
	struct pcn_kmsg_hdr header;
	int tid;
	int remote_ws;
	unsigned int size;
};

struct test_msg_response_t {
	struct pcn_kmsg_hdr header;
	int remote_ws;
};

static int handle_show_RW_dummy_buf(struct pcn_kmsg_message *inc_lmsg)
{
	_show_RW_dummy_buf(0);
	pcn_kmsg_done(inc_lmsg);

	return 0;
}

/*	FaRM
 * 	[we are here]
 *	compose
 *  send()
 *  *remap addr*
 *             ----->   irq (recv)
 *  poll                perform WRITE
 *  done				done
 *
 */
/* Perf data */
static int rdma_farm_test(unsigned int payload_size,
							unsigned long long iter, int t)
{
	unsigned long long i, j;

	for (j = 0; j < iter; j++) {
		for (i = 0; i < MAX_NUM_NODES; i++) {
			pcn_kmsg_perf_rdma_t *req_rdma;
			if (my_nid == i) continue;

			req_rdma = kmalloc(sizeof(*req_rdma), GFP_KERNEL);
			BUG_ON(!req_rdma);

			req_rdma->rdma_header.is_write = true;
			req_rdma->rdma_header.your_buf_ptr = dummy_act_buf[i][t];

			req_rdma->t_num = t;

			pcn_kmsg_request_rdma(PCN_KMSG_TYPE_RDMA_WRITE_TEST_REQUEST,
					i, req_rdma, sizeof(*req_rdma), payload_size);
			kfree(req_rdma);
			/* data is been written in dummy_act_buf[i][t] */
		}
	}
	return 0;
}

static int kthread_rdma_farm_test(void* arg0)
{
	struct test_params* karg = arg0;
	rdma_farm_test(karg->payload_size, karg->nr_iterations, karg->tid);
	atomic_inc(karg->nr_done);
	wake_up_interruptible(karg->wait_thread_sem);
	return 0;
}

/*	FaRM with 1 extra mem copy
 * 	[we are here]
 *	compose
 *  send()
 *  *remap addr*
 *             ----->   irq (recv)
 *  poll                perform WRITE
 *  return act_buf		done
 *
 */
/* real FaRM perf data */
static int rdma_farm_mem_cpy_test(unsigned int payload_size,
								unsigned long long iter, int t)
{
	unsigned long long i, j;
	for (j = 0; j < iter; j++) {
		for (i = 0; i < MAX_NUM_NODES; i++) {
			char *act_buf;
			pcn_kmsg_perf_rdma_t *req_rdma;
			if (my_nid == i) continue;

			req_rdma = kmalloc(sizeof(*req_rdma), GFP_KERNEL);
			BUG_ON(!req_rdma);

			//req_rdma->rdma_header.rmda_type_res =
			//			PCN_KMSG_TYPE_RDMA_WRITE_TEST_RESPONSE;
			//req_rdma->rdma_header.your_buf_ptr = dummy_act_buf[i][t];
			req_rdma->rdma_header.is_write = true;

			req_rdma->t_num = t; //xx also def
			act_buf = pcn_kmsg_request_rdma(
					PCN_KMSG_TYPE_RDMA_WRITE_TEST_REQUEST, i, req_rdma,
					sizeof(*req_rdma), payload_size);
			if (act_buf) {
#if POPCORN_DEBUG_MSG_IB
				int lengh = 0;

				printk("%s(): head length is 0x%.2x %.2x %.2x %.2x "
												"MUST BE %.8x(O)\n", __func__,
																*(act_buf + 0),
																*(act_buf + 1),
																*(act_buf + 2),
																*(act_buf + 3),
													payload_size);
				printk("%s(): is data 0x%.2x\n", __func__, *(act_buf + 4));
				printk("%s(): last byte 0x%.2x\n",
										__func__, *(act_buf+payload_size-1));

				lengh += *(act_buf + 0) << 24;
				lengh += *(act_buf + 1) << 16;
				lengh += *(act_buf + 2) << 8;
				lengh += *(act_buf + 3) << 0;
				printk("%s(): return int %d payload_size %llu\n\n",
											__func__, lengh, payload_size);
#endif
				//memcpy(dummy_act_buf[i][t], act_buf, payload_size); //usr time
				pcn_kmsg_done(act_buf);
			} else
				printk(KERN_WARNING "%s(): recv size 0\n", __func__);

			kfree(req_rdma);
		}
	}
	return 0;
}

static int kthread_rdma_farm_mem_cpy_test(void* arg0)
{
	struct test_params* karg = arg0;
	rdma_farm_mem_cpy_test(karg->payload_size, karg->nr_iterations, karg->tid);
	atomic_inc(karg->nr_done);
	wake_up_interruptible(karg->wait_thread_sem);
	return 0;
}

/*	FaRM2
 * 	[we are here]
 *	compose
 *  send()
 *  *remap addr*
 *             ----->   irq (recv)
 *   	                perform WRITE
 *  poll                perform WRITE
 *  done				done
 *
 */
/* real FaRM perf data */
static int rdma_farm2_data(unsigned int payload_size,
							unsigned long long iter, int t)
{
	unsigned long long i, j;
	for (j = 0; j < iter; j++) {
		for (i = 0; i < MAX_NUM_NODES; i++) {
			pcn_kmsg_perf_rdma_t *req_rdma;
			if (my_nid == i) continue;

			req_rdma = kmalloc(sizeof(*req_rdma), GFP_KERNEL);
			BUG_ON(!req_rdma);

			req_rdma->rdma_header.is_write = true;
			req_rdma->rdma_header.your_buf_ptr = dummy_act_buf[i][t];

			req_rdma->t_num = t; //xx also def

			pcn_kmsg_request_rdma(PCN_KMSG_TYPE_RDMA_WRITE_TEST_REQUEST,
					i, req_rdma, sizeof(*req_rdma), payload_size);

			/* data's been done in your_buf_ptr*/

			//memcpy(dummy_act_buf[i][t], act_buf, payload_size); //usr time
			kfree(req_rdma);
		}
	}
	return 0;
}

static int kthread_rdma_farm2_data(void* arg0)
{
	struct test_params* karg = arg0;
	rdma_farm2_data(karg->payload_size, karg->nr_iterations, karg->tid);
	atomic_inc(karg->nr_done);
	wake_up_interruptible(karg->wait_thread_sem);
	return 0;
}
#endif

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

static int test_rdma_dsm_rr(void *arg)
{
	int i;
	struct test_params *param = arg;
	ktime_t dt1, t1e, t1s;
	ktime_t t2e, t2s;
	ktime_t t3e, t3s;
	ktime_t t4e, t4s;
	ktime_t t5e, t5s;
	long long t2 = 0, t3 = 0, t4 = 0, t5 = 0;

#if 0
    remote_page_response_t *rp;
    struct wait_station *ws = get_wait_station(tsk);
    struct pcn_kmsg_rdma_handle *rh = NULL;
    remote_page_request_t *req; //

	// t1: get send buffer/rdma buffer from pool
    req = pcn_kmsg_get(sizeof(*req));
	req->origin_ws = ws->ws_id;

	rh = pcn_kmsg_pin_rdma_buffer(NULL, PAGE_SIZE);
	if (IS_ERR(rh)) {
		pcn_kmsg_put(req);
		return PTR_ERR(rh);
	}
	req->rdma_addr = rh->dma_addr;
	req->rdma_key = rh->rkey;

	// t1

	// t2
    pcn_kmsg_post(PCN_KMSG_TYPE_REMOTE_PAGE_REQUEST, //
					from_nid, req, sizeof(*req));
	// t2 end
    rp = wait_at_station(ws);
#endif

	//
	// write
	//
	DECLARE_COMPLETION_ONSTACK(done);
	test_dsmrr_request_t *req;
	struct pcn_kmsg_rdma_handle *rh;
///
		req = pcn_kmsg_get(sizeof(*req));
		rh = pcn_kmsg_pin_rdma_buffer(NULL, PAGE_SIZE);
		req->rdma_addr = rh->dma_addr;
		req->rdma_key = rh->rkey;
		req->size = param->payload_size;
		req->done = (unsigned long)&done;
		*(unsigned long *)rh->addr = 0xcafecaf00eadcafe; // touch. need?
		pcn_kmsg_send(PCN_KMSG_TYPE_TEST_RDMA_DSMRR_REQUEST,
								!my_nid, req, sizeof(*req));
		wait_for_completion(&done);
		pcn_kmsg_unpin_rdma_buffer(rh);
		pcn_kmsg_put(req);

///
	t1s = ktime_get();
	for (i = 0; i < param->nr_iterations; i++) {
		// t2
		t2s = ktime_get();
		req = pcn_kmsg_get(sizeof(*req));
		rh = pcn_kmsg_pin_rdma_buffer(NULL, PAGE_SIZE);
		req->rdma_addr = rh->dma_addr;
		req->rdma_key = rh->rkey;
		req->size = param->payload_size;
		req->done = (unsigned long)&done;
		//*(unsigned long *)rh->addr = 0xcafecaf00eadcafe; // touch. need?
		t2e = ktime_get();
		t2 += ktime_to_ns(ktime_sub(t2e, t2s));
		// t2

		// t3
		t3s = ktime_get();
		pcn_kmsg_post(PCN_KMSG_TYPE_TEST_RDMA_DSMRR_REQUEST,
								!my_nid, req, sizeof(*req));
		t3e = ktime_get();
		t3 += ktime_to_ns(ktime_sub(t3e, t3s));
		// t3

		// t4
		t4s = ktime_get();
		wait_for_completion(&done);
		t4e = ktime_get();
		t4 += ktime_to_ns(ktime_sub(t4e, t4s));
		// t4

		// t5
		t5s = ktime_get();
		//pcn_kmsg_put(req);
		pcn_kmsg_unpin_rdma_buffer(rh);
		t5e = ktime_get();
		t5 += ktime_to_ns(ktime_sub(t5e, t5s));
		// t5
	}
	t1e = ktime_get();
	dt1 = ktime_sub(t1e, t1s);
	//dt2 = ktime_sub(t2e, t2s);
	//dt3 = ktime_sub(t3e, t3s);
	//dt4 = ktime_sub(t4e, t4s);
	//dt5 = ktime_sub(t5e, t5s);
	printk("%s(): dsm rr lat done %lld ns %lld us!!!\n",
					__func__, ktime_to_ns(dt1) / param->nr_iterations,
					ktime_to_ns(dt1) / param->nr_iterations / 1000);
	printk("t2 %lld ns %lld us!!!\n",
					t2 / param->nr_iterations,
					t2 / param->nr_iterations / 1000);
	printk("t3 %lld ns %lld us!!!\n",
					t3 / param->nr_iterations,
					t3 / param->nr_iterations / 1000);
	printk("t4 %lld ns %lld us!!!\n",
					t4 / param->nr_iterations,
					t4 / param->nr_iterations / 1000);
	printk("t5 %lld ns %lld us!!!\n",
					t5 / param->nr_iterations,
					t5 / param->nr_iterations / 1000);


	printk("\n\n");

	//
	// send
	//
#if 0
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
#endif
	__barrier_wait(param->barrier);
	__barrier_wait(param->barrier);
	return 0;
}

void *buffer = NULL;
#define ITER 1000001
#define ONE_M 1000000
static void process_test_dsmrr_request(struct work_struct *work)
{

//  send example from DSM
//	START_KMSG_WORK(test_request_t, req, work);
//	if (test_bit(TEST_REQUEST_FLAG_REPLY, &req->flags)) {
//		test_response_t *res = pcn_kmsg_get(sizeof(*res));
//		res->done = req->done;
//
//		pcn_kmsg_post(PCN_KMSG_TYPE_TEST_RESPONSE,
//				PCN_KMSG_FROM_NID(req), res, sizeof(*res));
//	}
//	END_KMSG_WORK(req);

	int ret;
	START_KMSG_WORK(test_dsmrr_request_t, req, work);
	test_response_t *res;
	ktime_t t2e, t2s;
	ktime_t t3e, t3s;
	ktime_t t4e, t4s;
	ktime_t t5e, t5s;
	static long long t2 = 0, t3 = 0, t4 = 0, t5 = 0;
	static int cnt = 0;

	// tr2
	t2s = ktime_get();
	//res = kmalloc(sizeof(*res), GFP_KERNEL);
	res = pcn_kmsg_get(sizeof(*res));
	//*(unsigned long *)buffer = 0xbaffdeafbeefface; // touch. need?
	res->done = req->done;
	t2e = ktime_get();
	t2 += ktime_to_ns(ktime_sub(t2e, t2s));
	// tr2

	//tr3: directly any kernel_vaddr
	t3s = ktime_get();
	ret = pcn_kmsg_rdma_write(PCN_KMSG_FROM_NID(req),
			req->rdma_addr, buffer, req->size, req->rdma_key);
	t3e = ktime_get();
	t3 += ktime_to_ns(ktime_sub(t3e, t3s));
	//tr3

	//tr4
	t4s = ktime_get();
	pcn_kmsg_post(PCN_KMSG_TYPE_TEST_RDMA_DSMRR_RESPONSE,
	//pcn_kmsg_send(PCN_KMSG_TYPE_TEST_RDMA_DSMRR_RESPONSE,
					PCN_KMSG_FROM_NID(req), res, sizeof(*res));
	t4e = ktime_get();
	t4 += ktime_to_ns(ktime_sub(t4e, t4s));
	// had put *res
	//tr4

	//tr5
	//free_page((unsigned long)buffer);
	t5s = ktime_get();
	END_KMSG_WORK(req);
	t5e = ktime_get();
	t5 += ktime_to_ns(ktime_sub(t5e, t5s));
	//tr5

	cnt++;
	if (cnt <= 1) {
		t2 = 0, t3 = 0, t4 = 0, t5 = 0;
	}

	if (cnt >= ITER) {
        printk("%s(): t2 %lld ns %lld us!!!\n",
                        __func__,
                        t2 / ONE_M,
                        t2 / ONE_M / 1000);
        printk("%s(): t3 %lld ns %lld us!!!\n",
                        __func__,
                        t3 / ONE_M,
                        t3 / ONE_M / 1000);
        printk("%s(): t4 %lld ns %lld us!!!\n",
                        __func__,
                        t4 / ONE_M,
                        t4 / ONE_M / 1000);
        printk("%s(): t5 %lld ns %lld us!!!\n",
                        __func__,
                        t5 / ONE_M,
                        t5 / ONE_M / 1000);
	}
}

static int handle_test_dsmrr_response(struct pcn_kmsg_message *msg)
{
	//t5
	test_response_t *res = (test_response_t *)msg;
	if (res->done) {
		complete((struct completion *)res->done);
	}
	//t5
	pcn_kmsg_done(res);
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
	[TEST_ACTION_DSM_RR]		= { test_rdma_dsm_rr, "RDMA RR" },
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
	case TEST_ACTION_DSM_RR:
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
DEFINE_KMSG_WQ_HANDLER(test_dsmrr_request);

static struct proc_dir_entry *kmsg_test_proc = NULL;

static int __init msg_test_init(void)
{
	printk("\nLoading Popcorn messaging layer tester...\n");
	buffer = (void *)__get_free_page(GFP_KERNEL); // move to global

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
	REGISTER_KMSG_WQ_HANDLER(PCN_KMSG_TYPE_TEST_RDMA_DSMRR_REQUEST,
												test_dsmrr_request);
	REGISTER_KMSG_HANDLER(PCN_KMSG_TYPE_TEST_RDMA_DSMRR_RESPONSE,
												test_dsmrr_response);

	__show_usage();
	return 0;
}

static void __exit msg_test_exit(void)
{
	if (kmsg_test_proc) proc_remove(kmsg_test_proc);

	free_page((unsigned long)buffer);

	printk("Unloaded Popcorn messaging layer tester. Good bye!\n");
}

module_init(msg_test_init);
module_exit(msg_test_exit);
MODULE_LICENSE("GPL");
