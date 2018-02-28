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
#define DEFAULT_NR_ITERATIONS 1000

#if 0
/* for mimicing RW */
char *dummy_send_buf[MAX_NUM_NODES][MAX_THREADS];

/* For testing RDMA READ/WRITE */
char *dummy_act_buf[MAX_NUM_NODES][MAX_THREADS];
char *dummy_pass_buf[MAX_NUM_NODES][MAX_THREADS];
#endif

/* Buffers for testing RDMA RW */
int g_remote_read_len = 8 * 1024;
int g_rdma_write_len = 8 * 1024;
char *g_test_buf = NULL;
char *g_test_write_buf = NULL;

enum TEST_REQUEST_FLAG {
	TEST_REQUEST_FLAG_REPLY = 0,
};

#define TEST_REQUEST_FIELDS \
	unsigned long flags; \
	unsigned long done; \
	char msg[PCN_KMSG_MAX_PAYLOAD_SIZE -  \
		sizeof(unsigned long) * 2 \
	];
DEFINE_PCN_KMSG(test_request_t, TEST_REQUEST_FIELDS);

#define TEST_RESPONSE_FIELDS \
	unsigned long done;
DEFINE_PCN_KMSG(test_response_t, TEST_RESPONSE_FIELDS);

#define RDMA_TEST_FIELDS \
	int remote_ws; \
	u64 dma_addr_act; \
	u32 mr_id; \
	int t_num;
DEFINE_PCN_RDMA_KMSG(pcn_kmsg_perf_rdma_t, RDMA_TEST_FIELDS);

/*
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
*/

enum test_action {
	TEST_ACTION_SEND = 0,
	TEST_ACTION_SEND_WAIT,
	TEST_ACTION_POST,
	TEST_ACTION_RDMA_READ,
	TEST_ACTION_RDMA_WRITE,
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
 * Utility functions
 */
void setup_read_buf(void)
{
	// read specific (data you wanna let remote side to read)
	g_test_buf = kmalloc(g_remote_read_len, GFP_KERNEL);
	BUG_ON(!g_test_buf);
	memset(g_test_buf, 'R', g_remote_read_len);
							// user data buffer ( will be copied to rdma buf)
}

void setup_write_buf(void)
{
	// read specific (data you wanna let remote side to write)
	g_test_write_buf = kmalloc(g_rdma_write_len, GFP_KERNEL);
	BUG_ON(!g_test_write_buf);
	memset(g_test_write_buf, 'W', g_rdma_write_len);
							// user data buffer ( will be copied to rdma buf)
}

#if 0
void _show_RW_dummy_buf(int t)
{
	int j;
	for (j = 0; j < MAX_NUM_NODES; j++) {
		printk("CHECK active buffer\n"
						"_cb->rw_act_buf(first10) \"%.10s\"\n"
						"_cb->rw_act_buf(last 10) \"%.10s\"\n\n\n",
						dummy_act_buf[j][t],
						dummy_act_buf[j][t] + (MAX_MSG_LENGTH - 11));
		printk("CHECK pass buffe\n"
						"_cb->rw_pass_buf(first10) \"%.10s\"\n"
						"_cb->rw_pass_buf(last 10) \"%.10s\"\n\n\n",
						dummy_pass_buf[j][t],
						dummy_pass_buf[j][t] + (MAX_MSG_LENGTH - 11));
	}
}

void show_RW_dummy_buf(void)
{
	int i;
	struct pcn_kmsg_message *request; // youTODO: make your own struct

	_show_RW_dummy_buf(0);

	/* send to remote a request of  showing R/W buffers */
	request = kmalloc(sizeof(*request), GFP_KERNEL);
	BUG_ON(!request);

	for (i = 0; i < MAX_NUM_NODES; i++) {
		if (my_nid == i) continue;
		pcn_kmsg_send(PCN_KMSG_TYPE_SHOW_REMOTE_TEST_BUF, i, request, sizeof(*request));
	}

	kfree(request);
}

static int handle_show_RW_dummy_buf(struct pcn_kmsg_message *inc_lmsg)
{
	_show_RW_dummy_buf(0);
	pcn_kmsg_done(inc_lmsg);

	return 0;
}
*/

void init_RW_dummy_buf(int t)
{
	int j;
	printk("Init dummy buffer\n");
	for (j = 0; j < MAX_NUM_NODES; j++) {
		memset(dummy_act_buf[j][t], 'A', 10);
		memset(dummy_act_buf[j][t] + 10, 'B', MAX_MSG_LENGTH-10);
		memset(dummy_pass_buf[j][t], 'P', 10);
		memset(dummy_pass_buf[j][t] + 10, 'Q', MAX_MSG_LENGTH-10);
	}
}
#endif


/**
 * 	[we are here]
 *	[compose]
 *  send       ----->   irq (recv)
 *                      perform READ
 * irq (recv)  <-----   send
 */
static int test_rdma_read(void *arg)
{
	int i;

	pcn_kmsg_perf_rdma_t *req_rdma_read;
	req_rdma_read = kmalloc(sizeof(*req_rdma_read), GFP_KERNEL);

	req_rdma_read->rdma_header.rmda_type_res =
								PCN_KMSG_TYPE_RDMA_READ_TEST_RESPONSE;

	req_rdma_read->rdma_header.is_write = false;
	
	/* g_test_buf is allocated by setup_read_buf() */
	req_rdma_read->rdma_header.your_buf_ptr = g_test_buf;
	/*
	 * your buf will be copied to rdma buf for a passive remote read
	 * user should protect
	 * local buffer size for passive remote to read
	 */
	for (i = 0; i < MAX_NUM_NODES; i++) {
		if (my_nid == i) continue;
		pcn_kmsg_request_rdma(PCN_KMSG_TYPE_RDMA_READ_TEST_REQUEST,
				i, req_rdma_read, sizeof(*req_rdma_read), g_remote_read_len);
		printk("\n\n\n");
	}
	kfree(req_rdma_read);
	return 0;
}

/**
 * 	[we are here]
 *	[compose]
 *  send       ----->   irq (recv)
 *                      perform WRITE
 * irq (recv)  <-----   send
 */
static int test_rdma_write(void *arg)
{
	int i;

	pcn_kmsg_perf_rdma_t *req_rdma_write;
	req_rdma_write = kmalloc(sizeof(*req_rdma_write), GFP_KERNEL);

	req_rdma_write->rdma_header.rmda_type_res =
								PCN_KMSG_TYPE_RDMA_WRITE_TEST_RESPONSE;

	req_rdma_write->rdma_header.is_write = true;

	req_rdma_write->rdma_header.your_buf_ptr = g_test_write_buf;

	for (i = 0; i < MAX_NUM_NODES; i++) {
		if (my_nid == i) continue;
		pcn_kmsg_request_rdma(PCN_KMSG_TYPE_RDMA_WRITE_TEST_REQUEST,
				i, req_rdma_write, sizeof(*req_rdma_write), g_rdma_write_len);
	}
	kfree(req_rdma_write);
	return 0;
}


#if 0
// READ
static void process_handle_test_read_request(struct work_struct *_work)
{
	struct pcn_kmsg_work *work = (struct pcn_kmsg_work *)_work;
	pcn_kmsg_perf_rdma_t *req = work->msg;

	/* Prepare your *paddr */
	void* paddr = dummy_pass_buf[req->header.from_nid][req->t_num];

	/* RDMA routine */
	pcn_kmsg_respond_rdma(PCN_KMSG_TYPE_RDMA_READ_TEST_RESPONSE,
			req, paddr, req->rdma_header.rw_size);

	pcn_kmsg_done(req);
	kfree(work);
}

static void process_handle_test_read_response(struct work_struct *_work)
{
	struct pcn_kmsg_work *work = (struct pcn_kmsg_work *)_work;
	pcn_kmsg_perf_rdma_t *res = work->msg;
	struct wait_station *ws = wait_station(res->remote_ws);

	/* RDMA routine */
	pcn_kmsg_respond_rdma(PCN_KMSG_TYPE_RDMA_READ_TEST_RESPONSE, res, NULL, 0);

	ws->private = res;
	smp_mb();
	complete(&ws->pendings);

	// free outside
	kfree(work);
}

// WRITE
static void process_handle_test_write_request(struct work_struct *_work)
{
	struct pcn_kmsg_work *work = (struct pcn_kmsg_work *)_work;
	pcn_kmsg_perf_rdma_t *req = work->msg;

	/* Prepare your *paddr */
	void *paddr = dummy_pass_buf[req->header.from_nid][req->t_num];

	/* RDMA routine */
	pcn_kmsg_respond_rdma(PCN_KMSG_TYPE_RDMA_WRITE_TEST_RESPONSE, req, paddr, req->rdma_header.rw_size);

	pcn_kmsg_done(req);
	kfree(work);
}

static void process_handle_test_write_response(struct work_struct *_work)
{
	struct pcn_kmsg_work *work = (struct pcn_kmsg_work *)_work;
	pcn_kmsg_perf_rdma_t *res = work->msg;
	struct wait_station *ws = wait_station(res->remote_ws);

	/* RDMA routine */
	pcn_kmsg_respond_rdma(PCN_KMSG_TYPE_RDMA_WRITE_TEST_RESPONSE, res, NULL, 0);

	ws->private = res;
	smp_mb();
	complete(&ws->pendings);

	// free outside
	kfree(work);
}


/*
 * 	[we are here]
 *	compose
 *  send()
 *  *remap addr*
 *             ----->   irq (recv)
 *                      perform READ/WRITE
 * irq (recv)  <-----   send
 * 
 */
static int rdma_RW_test(unsigned int payload_size,
						unsigned long long iter, bool is_rdma_read, int t)
{
	unsigned long long i, j;

	for (j = 0; j < iter; j++) {
		for (i = 0; i < MAX_NUM_NODES; i++) {
			pcn_kmsg_perf_rdma_t *req_rdma;
			pcn_kmsg_perf_rdma_t *res;
			struct wait_station *ws;
			enum pcn_kmsg_type req_type;
			if (my_nid == i) continue;

			req_rdma = kmalloc(sizeof(*req_rdma), GFP_KERNEL);//xx
			BUG_ON(!req_rdma);

			ws = get_wait_station(current);
			req_rdma->remote_ws = ws->id;

			if (is_rdma_read == true) {
				req_type = PCN_KMSG_TYPE_RDMA_READ_TEST_REQUEST;
				req_rdma->rdma_header.rmda_type_res =
									PCN_KMSG_TYPE_RDMA_READ_TEST_RESPONSE;
			} else {
				req_type = PCN_KMSG_TYPE_RDMA_WRITE_TEST_REQUEST;
				req_rdma->rdma_header.rmda_type_res =
									PCN_KMSG_TYPE_RDMA_WRITE_TEST_RESPONSE;
			}

			req_rdma->header.prio = PCN_KMSG_PRIO_NORMAL;
			req_rdma->t_num = t;


			/* READ/WRITE specific */
			if (is_rdma_read == true)
				req_rdma->rdma_header.is_write = false;
			else
				req_rdma->rdma_header.is_write = true;

			req_rdma->rdma_header.your_buf_ptr = dummy_act_buf[i][t];

			pcn_kmsg_request_rdma(req_type, i, req_rdma, sizeof(*req_rdma), payload_size);
			kfree(req_rdma);

			res = wait_at_station(ws);
			put_wait_station(ws);
			/* data is in dummy_act_buf[i][t] */
			pcn_kmsg_done(res);
		}
	}
	return 0;
}


static int kthread_rdma_RW_test(void* arg0)
{
	struct test_params* karg = arg0;
	rdma_RW_test(karg->payload_size, karg->nr_iterations, karg->is_read, karg->tid);
	atomic_inc(karg->nr_done);
	wake_up_interruptible(karg->wait_thread_sem);
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

/*	mimic_read:
 *			=====>
 *					[copy]
 *			<-----
 */
void test_send_read_throughput(unsigned int payload_size,
								 unsigned long long iter, int tid)
{
	int i, dst;
	for (i = 0; i < iter; i++) {
		for (dst = 0; dst < MAX_NUM_NODES; dst++) {
			struct wait_station *ws;
			struct mimic_rw_msg_request *req;
			struct mimic_rw_signal_request *res;
			if (my_nid == dst) continue;
			req = kmalloc(sizeof(*req), GFP_KERNEL);
			BUG_ON(!req);

			ws = get_wait_station(current);
			req->remote_ws = ws->id;
			req->size = payload_size;
			req->tid = tid;
			//printk("mimic READ (cost for msg layer)\n");
			memcpy(&req->payload, dummy_send_buf[dst][tid], payload_size);

			printk("%s(): r local to remote size %llu = "
							"sizeof(*req)(%lu) - sizeof(req->payload)(%lu)"
							" + payload_size(%llu)\n", __func__,
							sizeof(*req) - sizeof(req->payload) + payload_size,
							sizeof(*req), sizeof(req->payload), payload_size);

			pcn_kmsg_send(PCN_KMSG_TYPE_SEND_ROUND_READ_REQUEST, dst, req,
						sizeof(*req) - sizeof(req->payload) + payload_size);

			kfree(req);
			res = wait_at_station(ws);
			put_wait_station(ws);

			pcn_kmsg_done(res);
		}
	}
}

/*	mimic_write:	(more often)
 *			----->
 *			<====
 *	 [copy]
 *
 */
void test_send_write_throughput(unsigned int payload_size,
								unsigned long long iter, int tid)
{
	int i, dst;
	for (i = 0; i < iter; i++) {
		for (dst = 0; dst < MAX_NUM_NODES; dst++) {
			struct wait_station *ws;
			struct mimic_rw_signal_request *req;
			struct mimic_rw_msg_request *res;
			if (my_nid == dst) continue;
			req = kmalloc(sizeof(struct mimic_rw_signal_request), GFP_KERNEL);
			BUG_ON(!req);

			ws = get_wait_station(current);
			req->remote_ws = ws->id;
			req->size = payload_size;
			req->tid = tid;

			pcn_kmsg_send(PCN_KMSG_TYPE_SEND_ROUND_WRITE_REQUEST,
					dst, req, sizeof(*req));

			kfree(req);
			res = wait_at_station(ws);
			put_wait_station(ws);

			//printk("mimic WRITE (cost for usr)\n");
			//memcpy(dummy_send_buf[dst][tid], &res->payload, payload_size);

			pcn_kmsg_done(res);
		}
	}
}

static int kthread_test_send_rw_throughput(void* arg0)
{
	struct test_params* karg = arg0;
	if (karg->is_read)
		test_send_read_throughput(karg->payload_size, karg->nr_iterations, karg->tid);
	else
		test_send_write_throughput(karg->payload_size, karg->nr_iterations, karg->tid);
	atomic_inc(karg->nr_done);
	wake_up_interruptible(karg->wait_thread_sem);
	return 0;
}

static int rdma_RW_inv_test(void* buf, unsigned int payload_size,
							bool is_rdma_read, int t)
{
	unsigned long long i;

	enum pcn_kmsg_type req_type;
	pcn_kmsg_perf_rdma_t *req_rdma;
	req_rdma = kmalloc(sizeof(*req_rdma), GFP_KERNEL);
	if (!req_rdma)
		return -1;

	if (is_rdma_read == true) {
		req_type = PCN_KMSG_TYPE_RDMA_READ_TEST_REQUEST;
		req_rdma->rdma_header.rmda_type_res =
								PCN_KMSG_TYPE_RDMA_READ_TEST_RESPONSE;
	} else {
		req_type = PCN_KMSG_TYPE_RDMA_WRITE_TEST_REQUEST;
		req_rdma->rdma_header.rmda_type_res =
								PCN_KMSG_TYPE_RDMA_WRITE_TEST_RESPONSE;
	}
	req_rdma->header.prio = PCN_KMSG_PRIO_NORMAL;

	/* READ/WRITE specific */
	if (is_rdma_read == true)
		req_rdma->rdma_header.is_write = false;
	else
		req_rdma->rdma_header.is_write = true;

	req_rdma->rdma_header.your_buf_ptr = buf;	// provided by user

	req_rdma->t_num = t;

	for (i = 0; i < MAX_NUM_NODES; i++) {
		if (my_nid == i) continue;
		pcn_kmsg_request_rdma(req_type, i,
				req_rdma, sizeof(*req_rdma), payload_size);
		printk("\n\n\n");
	}
	msleep(1000); // wait for taking effect

	kfree(req_rdma);
	return 0;
}

static void __reply_send_w_roundtrip(struct mimic_rw_signal_request *req,
									int ret)
{
	struct mimic_rw_msg_request *res = kmalloc(sizeof(*res), GFP_KERNEL);
	res->remote_ws = req->remote_ws;
	res->size = req->size;

	printk("%s(): w remote back size %lu = "
				"sizeof(*res)%lu - sizeof(res->payload)%lu + req->size %d\n",
				__func__, sizeof(*res) - sizeof(res->payload) + req->size,
							sizeof(*res), sizeof(res->payload), req->size);

	//printk("mimic WRITE (cost for msg layer)\n");
	memcpy(&res->payload,
			dummy_send_buf[req->header.from_nid][req->tid], req->size);
	pcn_kmsg_send(PCN_KMSG_TYPE_SEND_ROUND_WRITE_RESPONSE, req->header.from_nid, res,
					sizeof(*res) - sizeof(res->payload) + req->size);
	kfree(res);
}

static void process_send_roundtrip_w_request(struct work_struct *_work)
{
	struct pcn_kmsg_work *work = (struct pcn_kmsg_work *)_work;
	struct mimic_rw_signal_request *req = work->msg;

	__reply_send_w_roundtrip(req, -EINVAL);

	pcn_kmsg_done(req);
	kfree(work);
}

static void process_send_roundtrip_w_response(struct work_struct *_work)
{
	struct pcn_kmsg_work *work = (struct pcn_kmsg_work *)_work;
	struct mimic_rw_msg_request *res = work->msg;
	struct wait_station *ws = wait_station(res->remote_ws);

	ws->private = res;
	smp_mb();
	complete(&ws->pendings);

	//free outside
	kfree(work);
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

	req = kmalloc(sizeof(*req), GFP_KERNEL);
	BUG_ON(!req);
	prandom_bytes(&req->msg, sizeof(req->msg));
	req->flags = 0;
	if (param->action == TEST_ACTION_SEND_WAIT) {
		set_bit(TEST_REQUEST_FLAG_REPLY, &req->flags);
		req->done = (unsigned long)&done;
	}

	__barrier_wait(param->barrier);
	for (i = 0; i < param->nr_iterations; i++) {
		pcn_kmsg_send(PCN_KMSG_TYPE_TEST_REQUEST,
				!my_nid, req, PCN_KMSG_SIZE(param->payload_size));

		if (param->action == TEST_ACTION_SEND_WAIT) {
			wait_for_completion(&done);
		}
	}
	__barrier_wait(param->barrier);

	kfree(req);
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

		set_bit(TEST_REQUEST_FLAG_REPLY, &req->flags);
		req->done = (unsigned long)&done;

		pcn_kmsg_post(PCN_KMSG_TYPE_TEST_REQUEST,
				!my_nid, req, PCN_KMSG_SIZE(param->payload_size));

		wait_for_completion(&done);
	}
	__barrier_wait(param->barrier);
	return 0;
}

static void process_test_send_request(struct work_struct *_work)
{
	struct pcn_kmsg_work *work = (struct pcn_kmsg_work *)_work;
	test_request_t *req = work->msg;

	if (test_bit(TEST_REQUEST_FLAG_REPLY, &req->flags)) {
		test_response_t *res = pcn_kmsg_get(sizeof(*res));
		res->done = req->done;

		pcn_kmsg_post(PCN_KMSG_TYPE_TEST_RESPONSE, PCN_KMSG_FROM_NID(req),
				res, sizeof(*res));
	}

	pcn_kmsg_done(req);
	kfree(work);
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


struct test_desc {
	int (*test_fn)(void *);
	char *description;
};

static struct test_desc tests[] = {
	[TEST_ACTION_SEND]			= { test_send, "synchronous send"  },
	[TEST_ACTION_SEND_WAIT]		= { test_send, "synchronous send with wait" },
	[TEST_ACTION_POST]			= { test_post, "synchronous post" },

	[TEST_ACTION_RDMA_READ]		= { test_rdma_read, "RDMA read" },
	[TEST_ACTION_RDMA_WRITE]	= { test_rdma_write, "RDMA write" },
};

static void __run_test(enum test_action action, struct test_params *param)
{
	struct test_params thread_params[MAX_THREADS] = {};
	struct task_struct *tsks[MAX_THREADS] = { NULL };
	struct test_barrier barrier;
	struct timeval t_start, t_end;
	DECLARE_COMPLETION_ONSTACK(done);
	unsigned long elapsed;
	int i;

	__barrier_init(&barrier, param->nr_threads + 1);
	param->barrier = &barrier;
	//param->is_read = (action == 13) || (action == 15) ? true : false;

	for (i = 0; i < param->nr_threads; i++) {
		struct test_params *thr_param = thread_params + i;

		*thr_param = *param;
		thr_param->tid = i;

		tsks[i] = kthread_run(tests[action].test_fn, thr_param, "test_%d", i);
	}

	__barrier_wait(&barrier);
	do_gettimeofday(&t_start);
	/* run the test */
	__barrier_wait(&barrier);
	do_gettimeofday(&t_end);


	elapsed = (t_end.tv_sec * 1000000 + t_end.tv_usec) -
			(t_start.tv_sec * 1000000 + t_start.tv_usec);

	printk("Done testing %s\n", tests[action].description);
	printk("  %u thread%s %lu iteration%s\n",
			param->nr_threads, param->nr_threads == 1 ? "" : "s",
			param->nr_iterations, param->nr_iterations == 1 ? "" : "s");
	printk("  %9lu us in total\n", elapsed);
	printk("  %3lu.%05lu us per operation\n",
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

	if (args >= 2)
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
	case TEST_ACTION_SEND_WAIT:
	case TEST_ACTION_POST:
		__run_test(action, &params);
		break;
	case TEST_ACTION_RDMA_READ:
		setup_read_buf();
		__run_test(action, &params);
		break;
	case TEST_ACTION_RDMA_WRITE:
		setup_write_buf();
		__run_test(action, &params);
		break;
	default:
		printk("Unknown test no #%d\n", action);
	}
#if 0
	else if (cmd[0] == '9' && cmd[1] == '\0') {
		setup_read_buf();
		for (i = 0; i < 10; i++) {
			t = kthread_run(kthread_test1, NULL, "kthread_test1()");
			BUG_ON(IS_ERR(t));
			t = kthread_run(kthread_test2, NULL, "kthread_test2()");
			BUG_ON(IS_ERR(t));
			t = kthread_run(kthread_test3, NULL, "kthread_test3()");
			BUG_ON(IS_ERR(t));
		}
	}
	else if (!memcmp(argv[0], "11", 2) || !memcmp(argv[0], "12", 2)) {
		if (args != 4) {
			karg.isread = !memcmp(argv[0], "11", 2) ? true : false;
			for (i = 0; i < karg.nr_threads; i++) {
				t = kthread_run(kthread_test_send_rw_throughput, karg_ptrs[i],
								"kthread_test_send_rw_throughput()");
				BUG_ON(IS_ERR(t));
			}
			wait_event_interruptible(wait_thread_sem,
					atomic_read(&thread_done_cnt) >= karg.nr_threads);
			EXP_LOG("test %s %s (M); test_send_roundtrip_throughput() done\n",
					argv[0], !memcmp(argv[0], "11", 2) ?
					"Mimic READ" : "Mimic WRITE");
		} else if (args != 3) {
			if (karg.iter == 0)
				printk(KERN_WARNING "iter = 0\n");

			for (i = 0; i < ITER; i++) {
				if (!memcmp(argv[0], "11", 2))
					test_send_read_throughput(karg.payload_size, karg.iter, 0);
				else
					test_send_write_throughput(karg.payload_size, karg.iter, 0);
			}
			EXP_LOG("test %s %s: test_send_roundtrip_throughput() done\n",
					argv[0], !memcmp(argv[0], "11", 2) ?
					"Mimic READ" : "Mimic WRITE");
		} else {
			show_usage();
			goto exit;
		}
	}
	else if (!memcmp(argv[0], "13", 2) || !memcmp(argv[0], "14", 2)) {
		if (args != 4) {
			karg.isread = !memcmp(argv[0], "13", 2) ? true : false;
			for (i = 0; i < karg.nr_threads; i++) {
				t = kthread_run(kthread_rdma_RW_test, karg_ptrs[i],
								"kthread_rdma_RW_test()");
				BUG_ON(IS_ERR(t));
			}
			wait_event_interruptible(wait_thread_sem,
					atomic_read(&thread_done_cnt) >= karg.nr_threads);
			EXP_LOG("RDMA %s (M): rdma_RW_test() done\n\n\n",
					!memcmp(argv[0], "13", 2) ? "READ" : "WRITE");
		} else if (args != 3 ) {
			rdma_RW_test(karg.payload_size, karg.iter,
						!memcmp(argv[0], "13", 2) ? true : false, 0);
			EXP_LOG("RDMA %s rdma_RW_test() single thread done\n\n\n",
					!memcmp(argv[0], "13", 2) ? "READ" : "WRITE");
		} else {
			show_usage();
			goto exit;
		}
	}
	else if (!memcmp(argv[0], "15", 2)) {
		/* Because of this LENGTH is not divided by 10,
		 * the last few chars are not changed!			*/
		int z, ofs = MAX_MSG_LENGTH/10;

		// ("----- READ test sanity start -----\n");

		/* init act_buf */
		for (z = 0; z < 10; z++) {
			memset(dummy_act_buf[0][0]+(z*ofs), z+'a', ofs);
			printk("z %d act_buf \"%.10s\"\n", z, dummy_act_buf[0][0]+(z*ofs));
		}

		for (z = 0; z < 10; z++) {
			rdma_RW_inv_test(dummy_act_buf[0][0]+(z*ofs), ofs, 1, 0);
			show_RW_dummy_buf();
			msleep(3000);
		}

		// ("----- READ test sanity done -----\n");
		msleep(5000);
		init_RW_dummy_buf(0);
		show_RW_dummy_buf();
		msleep(5000);

		// ("----- WRITE test sanity start ----\n");

		for (z = 0; z < 10; z++) {
			rdma_RW_inv_test(dummy_act_buf[0][0]+(z*ofs), ofs, 0, 0);
			show_RW_dummy_buf();
			msleep(3000);
		}
		// ("----- WRITE test sanity done -----\n");
	} else if (!memcmp(argv[0], "16", 2)) {
		init_RW_dummy_buf(0);
	} else if (!memcmp(argv[0], "17", 2)) {
		show_RW_dummy_buf();
	} else if (!memcmp(argv[0], "20", 2)) {
		if (4 == args) {
			for (i = 0; i < karg.nr_threads; i++) {
				t = kthread_run(kthread_rdma_farm_test, karg_ptrs[i],
								"kthread_rdma_farm_test()");
				BUG_ON(IS_ERR(t));
			}
			wait_event_interruptible(wait_thread_sem,
					atomic_read(&thread_done_cnt) >= karg.nr_threads);
			EXP_LOG("RDMA %c (M): FaRM RDMA WRITE "
					"rdma_farm_test() done\n", cmd[0]);
		} else if (3 == args) {
			rdma_farm_test(karg.payload_size, karg.iter, 0);
			EXP_LOG("RDMA %c: FaRM RDMA WRITE "
					"rdma_farm_test() done\n", cmd[0]);
		} else {
			show_usage();
			goto exit;
		}
	} else if (!memcmp(argv[0], "21", 2)) {
		if (4 == args) {
			for (i = 0; i < karg.nr_threads; i++) {
				t = kthread_run(kthread_rdma_farm_mem_cpy_test, karg_ptrs[i],
								"kthread_rdma_farm_mem_cpy_test()");
				BUG_ON(IS_ERR(t));
			}
			wait_event_interruptible(wait_thread_sem,
					atomic_read(&thread_done_cnt) >= karg.nr_threads);
			EXP_LOG("RDMA %c (M): FaRM RDMA WRITE "
					"rdma_farm_mem_cpy_test() done\n", cmd[0]);
		} else if (3 == args) {
			rdma_farm_mem_cpy_test(karg.payload_size, karg.iter, 0);
			EXP_LOG("RDMA %c: FaRM RDMA WRITE "
					"rdma_farm_mem_cpy_test() done\n", cmd[0]);
		} else {
			show_usage();
			goto exit;
		}
	} else if (!memcmp(argv[0], "22", 2)) {
		if (4 == args) {
			for (i = 0; i < karg.nr_threads; i++) {
				t = kthread_run(kthread_rdma_farm2_data, karg_ptrs[i],
								"kthread_rdma_farm2_data()");
				BUG_ON(IS_ERR(t));
			}
			wait_event_interruptible(wait_thread_sem,
					atomic_read(&thread_done_cnt) >= karg.nr_threads);
			EXP_LOG("RDMA %c (M): FaRM RDMA WRITE "
					"rdma_farm2_data() done\n", cmd[0]);
		} else if (3 == args) {
			rdma_farm2_data(karg.payload_size, karg.iter, 0);
			EXP_LOG("RDMA %c: FaRM RDMA WRITE "
					"rdma_farm2_data() done\n", cmd[0]);
		} else {
			show_usage();
			goto exit;
		}
	} else {
		printk("Not support yet. Try \"1,2,3,4,5,6,9,10\"\n");
	}
	do_gettimeofday(&t_end);
#endif

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
/*
DEFINE_KMSG_WQ_HANDLER(send_roundtrip_r_request);
DEFINE_KMSG_WQ_HANDLER(send_roundtrip_r_response);

DEFINE_KMSG_WQ_HANDLER(send_roundtrip_w_request);
DEFINE_KMSG_WQ_HANDLER(send_roundtrip_w_response);

DEFINE_KMSG_WQ_HANDLER(handle_test_read_request);
DEFINE_KMSG_WQ_HANDLER(handle_test_read_response);

DEFINE_KMSG_WQ_HANDLER(handle_test_write_request);
DEFINE_KMSG_WQ_HANDLER(handle_test_write_response);
*/

static struct proc_dir_entry *kmsg_test_proc = NULL;

static int __init msg_test_init(void)
{
	printk("Loading Popcorn messaging layer tester...\n");

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

#if 0
	/* init dummy buffers for geting experimental data */
	for (j = 0; j < MAX_NUM_NODES; j++) {
		for (i = 0; i < MAX_THREADS; i++) {
			dummy_send_buf[j][i] = kzalloc(MAX_MSG_LENGTH, GFP_KERNEL);
			dummy_act_buf[j][i] = kzalloc(MAX_MSG_LENGTH, GFP_KERNEL);
			dummy_pass_buf[j][i] = kzalloc(MAX_MSG_LENGTH, GFP_KERNEL);
			BUG_ON (!dummy_send_buf[j][i] ||
					!dummy_act_buf[j][i] ||
					!dummy_pass_buf[j][i]);
			memset(dummy_send_buf[j][i], 'S', MAX_MSG_LENGTH / 2);
			memset(dummy_send_buf[j][i] + (MAX_MSG_LENGTH / 2),
										'T', MAX_MSG_LENGTH / 2);
			memset(dummy_act_buf[j][i], 'A', 10);
			memset(dummy_act_buf[j][i] + 10, 'B', MAX_MSG_LENGTH - 10);
			memset(dummy_pass_buf[j][i], 'P', 10);
			memset(dummy_pass_buf[j][i] + 10, 'Q', MAX_MSG_LENGTH - 10);
		}
	}
	*/

	/* for experimental data - send throughput */
	REGISTER_KMSG_HANDLER(PCN_KMSG_TYPE_SHOW_REMOTE_TEST_BUF, show_RW_dummy_buf);

	/* for experimental data - Round-trip throughput */
	REGISTER_KMSG_WQ_HANDLER(PCN_KMSG_TYPE_SEND_ROUND_READ_REQUEST,
										send_roundtrip_r_request);
	REGISTER_KMSG_WQ_HANDLER(PCN_KMSG_TYPE_SEND_ROUND_READ_RESPONSE,
										send_roundtrip_r_response);

	REGISTER_KMSG_WQ_HANDLER(PCN_KMSG_TYPE_SEND_ROUND_WRITE_REQUEST,
										send_roundtrip_w_request);
	REGISTER_KMSG_WQ_HANDLER(PCN_KMSG_TYPE_SEND_ROUND_WRITE_RESPONSE,
										send_roundtrip_w_response);

	/* for experimental data - READ/WRITE throughput */
	REGISTER_KMSG_WQ_HANDLER(PCN_KMSG_TYPE_RDMA_READ_TEST_REQUEST,
										handle_test_read_request);
	REGISTER_KMSG_WQ_HANDLER(PCN_KMSG_TYPE_RDMA_READ_TEST_RESPONSE,
										handle_test_read_response);

	REGISTER_KMSG_WQ_HANDLER(PCN_KMSG_TYPE_RDMA_WRITE_TEST_REQUEST,
										handle_test_write_request);
	REGISTER_KMSG_WQ_HANDLER(PCN_KMSG_TYPE_RDMA_WRITE_TEST_RESPONSE,
										handle_test_write_response);
#endif

	__show_usage();
	return 0;
}

static void __exit msg_test_exit(void) 
{
	if (kmsg_test_proc) proc_remove(kmsg_test_proc);

	/*
	for (j = 0; j < MAX_NUM_NODES; j++) {
		for (i = 0; i < MAX_THREADS; i++) {
			kfree(dummy_act_buf[j][i]);
			kfree(dummy_pass_buf[j][i]);
		}
	}
	if (g_test_buf)
		kfree(g_test_buf);

	if (g_test_write_buf)
		kfree(g_test_write_buf);
	*/

	printk("Unloaded Popcorn messaging layer tester. Good bye!\n");
}

module_init(msg_test_init);
module_exit(msg_test_exit);
MODULE_LICENSE("GPL");
