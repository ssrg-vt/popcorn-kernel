/*
 * Copyright (C) 2017 JackChuang <horenc@vt.edu>
 * 
 *TODO:
 * 		- write_proc should be protected by a muxtex
 */
#include <linux/slab.h>
#include <linux/delay.h>
#include <asm/uaccess.h>
#include <linux/module.h>
#include <linux/string.h>
#include <linux/kthread.h>
#include <linux/vmalloc.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>

#include <popcorn/debug.h>
#include <popcorn/bundle.h>
#include <popcorn/pcn_kmsg.h>

/* rdma */
#include <rdma/ib_verbs.h>
#include <rdma/rdma_cm.h>
/* page */
#include <linux/pagemap.h>

/* wait station */
#include "../../kernel/popcorn/types.h"
#include "../../kernel/popcorn/wait_station.h"

#include "common.h"

#define MAX_MSG_LENGTH PCN_KMSG_LONG_PAYLOAD_SIZE	/* 1 bit for FaRM */

/* testing args */
#define ITER 1					/* iter for test10 (send throughput) */
#define MAX_ARGS_NUM 10
#define TEST1_MSG_COUNT 100000	/* specifically for test1 iterations */

/* Testing args for multi-threading */
#define MAX_CONCURRENT_THREADS 16

#define MAX_TESTING_SIZE 120 * 1024 * 1024
#define TEST1_PAYLOAD_SIZE 1024

/* for mimicing RW */
char *dummy_send_buf[MAX_NUM_NODES][MAX_CONCURRENT_THREADS];

/* For testing RDMA READ/WRITE */
char *dummy_act_buf[MAX_NUM_NODES][MAX_CONCURRENT_THREADS];
char *dummy_pass_buf[MAX_NUM_NODES][MAX_CONCURRENT_THREADS];

extern void usr_rdma_poll_done(int);

/* Buffers for testing RDMA RW */
int g_remote_read_len = 8 * 1024;
int g_rdma_write_len = 8 * 1024;
char *g_test_buf = NULL;
char *g_test_write_buf = NULL;

/* getting performance data */
#define POPCORN_EXP_DATA_MSG_IB 1
#if POPCORN_EXP_DATA_MSG_IB
#define EXP_DATA(...) printk(__VA_ARGS__)
#else
#define EXP_DATA(...)
#endif 

/* making sure it's done */
#define POPCORN_EXP_LOG_MSG_IB 0
#if POPCORN_EXP_LOG_MSG_IB
#define EXP_LOG(...) printk(__VA_ARGS__)
#else
#define EXP_LOG(...)
#endif 

/* general debug logs */
#define POPCORN_DEBUG_MSG_IB 0
#if POPCORN_DEBUG_MSG_IB
#define MSG_RDMA_PRK(...) printk(__VA_ARGS__)
#define KRPRINT_INIT(...) printk(__VA_ARGS__)
#define MSG_SYNC_PRK(...) printk(__VA_ARGS__)
#define DEBUG_LOG_V(...) printk(__VA_ARGS__)
#define DEBUG_CORRECTNESS(...) printk(__VA_ARGS__)
//#define DEBUG_LOG_V(...) trace_printk(__VA_ARGS__)
#else
#define MSG_RDMA_PRK(...)
#define KRPRINT_INIT(...)
#define MSG_SYNC_PRK(...)
#define DEBUG_LOG_V(...)
#define DEBUG_CORRECTNESS(...)
#endif 

/* example - data structure (dbg info) */
typedef struct {
	struct pcn_kmsg_hdr header; /* must follow */
	struct pcn_kmsg_rdma_hdr rdma_header;
	/* you define */
	int example1;
	int example2;
#ifdef CONFIG_POPCORN_DEBUG_MSG_LAYER_VERBOSE
#endif  
	char msg[TEST1_PAYLOAD_SIZE];
}__attribute__((packed)) test_request_t;

struct test_msg_t {
	struct pcn_kmsg_hdr header;
	unsigned char payload[MAX_MSG_LENGTH];
};

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

struct kmsg_arg {
	int tid;
	u64 iter;
	bool isread;
	u64	payload_size;
	atomic_t *thread_done_cnt;
	wait_queue_head_t *wait_thread_sem;
};

void show_instruction(void)
{
	printk("--- Popcorn messaging layer self-testing proc init done ---\n");
	printk("--- Usage: sudo echo [NUM] [DEPENDS] > /proc/kmsg_test ---\n");
	printk("--- [S]: Single thread test\n");
	printk("--- [M]: Multithreading test t = %d\n", MAX_CONCURRENT_THREADS);
	printk("==================== sanity check ====================\n");
	printk("---  1: continuously send/recv test ---\n");
	printk("---  2: continuously READ test ---\n");
	printk("---  3: continuously WRITE test ---\n");
	printk("---  4: continuously multithreading send/recv test ---\n");
	printk("---  5: continuously multithreading READ test ---\n");
	printk("---  6: continuously multithreading WRITE test ---\n");
	printk("---  9: continuously multithreading "
									"send/recv/READ/WRITE test ---\n");
	printk("---      ex: echo [NUM] > /proc/kmsg_test---\n");
	printk("==================== experimental data ====================\n");
	printk("---  10: single thread send throughput (one way) ---\n");
	printk("---      ex: echo 10 [SIZE] > /proc/kmsg_test ---\n");
	printk("---  11: single thread send throughput (round-trip - "
									"simulate RDMA READ) ---\n");
	printk("---      ex: echo 11 [SIZE] [ITER] (M) > /proc/kmsg_test ---\n");
	printk("---  12: single thread send throughput (round-trip - "
									"simulate RDMA WRITE) ---\n");
	printk("---      ex: echo 12 [SIZE] [ITER] (M) > /proc/kmsg_test ---\n");
	printk("---  13: RDMA READ a page throughput  ---\n");
	printk("---      ex: echo 13 [SIZE] [ITER] (M) > /proc/kmsg_test ---\n");
	printk("---  14: RDMA WRITE a page throughput  ---\n");
	printk("---      ex: echo 14 [SIZE] [ITER] (M) > /proc/kmsg_test ---\n");
	printk("---  15: RDMA READ invalidation test (10 buf of 8192 size) ---\n");
	printk("---      ex: echo 15 > /proc/kmsg_test ---\n");
	printk("---  16: re-init RDMA RW testing buffers  ---\n");
	printk("---      ex: echo 16 > /proc/kmsg_test ---\n");
	printk("---  17: show RDMA RW testing buffers  ---\n");
	printk("---      ex: echo 17 > /proc/kmsg_test ---\n");
	printk("---  20: FaRM RDMA WRITE w/o memory copying (prelim data) ---\n");
	printk("---      ex: echo 20 [SIZE] [ITER] (M) > /proc/kmsg_test ---\n");
	printk("---  21: FaRM RDMA WRITE w/ memory copying ---\n");
	printk("---      ex: echo 21 [SIZE] [ITER] (M) > /proc/kmsg_test ---\n");
	printk("---  22: FaRM RDMA WRITE w/ 2 WRITE ---\n");
	printk("---      ex: echo 22 [SIZE] [ITER] (M) > /proc/kmsg_test ---\n");
	printk("=============== msg_layer usage pattern  ===============\n");
	printk("---  cat: showing msg_layer usage pattern ---\n");
	printk("---      ex: cat /proc/kmsg_test ---\n");
	printk("\n\n\n\n\n\n\n\n");
}

bool cmp_args(int a, int b)
{
	return a == b? true: false;
}

/* testing utility */
/*
 * in the reality, this function should be called between send()s
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

void _show_RW_dummy_buf(int t)
{
	int j;
	for (j = 0; j < MAX_NUM_NODES; j++) {
		printk("<<<<< CHECK active buffer >>>>> \n"
						"_cb->rw_act_buf(first10) \"%.10s\"\n"
						"_cb->rw_act_buf(last 10) \"%.10s\"\n\n\n",
						dummy_act_buf[j][t],
						dummy_act_buf[j][t] + (MAX_MSG_LENGTH - 11));
		printk("<<<<< CHECK pass buffer>>>>> \n"
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
	request = pcn_kmsg_alloc_msg(sizeof(*request));
	BUG_ON(!request);

	request->header.type = PCN_KMSG_TYPE_SHOW_REMOTE_TEST_BUF;
	request->header.prio = PCN_KMSG_PRIO_NORMAL;

	for (i = 0; i < MAX_NUM_NODES; i++) {
		if (my_nid == i) continue;
		pcn_kmsg_send(i, request, sizeof(*request));
	}

	pcn_kmsg_free_msg(request);
}

static void handle_show_RW_dummy_buf( struct pcn_kmsg_message *inc_lmsg)
{
	_show_RW_dummy_buf(0);
	pcn_kmsg_free_msg(inc_lmsg);
}

void init_RW_dummy_buf(int t)
{
	int j;
	printk("---------------------------\n");
	printk("----- init dummy buff -----\n");
	printk("---------------------------\n");
	for (j = 0; j < MAX_NUM_NODES; j++) {
		memset(dummy_act_buf[j][t], 'A', 10);
		memset(dummy_act_buf[j][t] + 10, 'B', MAX_MSG_LENGTH-10);
		memset(dummy_pass_buf[j][t], 'P', 10);
		memset(dummy_pass_buf[j][t] + 10, 'Q', MAX_MSG_LENGTH-10);
	}
}

static struct mimic_rw_msg_request *
				__alloc_send_roundtrip_read_request(void)
{
	struct mimic_rw_msg_request *req = pcn_kmsg_alloc_msg(sizeof(*req));
	BUG_ON (!req);

	req->header.type = PCN_KMSG_TYPE_SEND_ROUND_READ_REQUEST;
	req->header.prio = PCN_KMSG_PRIO_NORMAL;
	return req;
}


static struct mimic_rw_signal_request *
			__alloc_send_roundtrip_write_request(void)
{
	struct mimic_rw_signal_request *req =
			pcn_kmsg_alloc_msg(sizeof(struct mimic_rw_signal_request));
	if (!req)
		return NULL;

	req->header.type = PCN_KMSG_TYPE_SEND_ROUND_WRITE_REQUEST;
	req->header.prio = PCN_KMSG_PRIO_NORMAL;
	return req;
}

void _show_time(struct timeval *t1, struct timeval *t2,
				unsigned long long payload_size,
				unsigned long long iter, char *str)
{
	EXP_DATA("%s: size %llu, iter %llu, %ld.%06ld\n",
						str, payload_size, iter,

						t2->tv_usec-t1->tv_usec >= 0 ?
						t2->tv_sec-t1->tv_sec :
						t2->tv_sec-t1->tv_sec-1,

						t2->tv_usec-t1->tv_usec >= 0 ?
						t2->tv_usec-t1->tv_usec :
						(1000000-(t1->tv_usec-t2->tv_usec)));
}




/* example - handler */
static void handle_remote_thread_first_test_request(
									struct pcn_kmsg_message* inc_lmsg)
{
	test_request_t* request = 
						(test_request_t*) inc_lmsg;

#ifdef CONFIG_POPCORN_DEBUG_MSG_LAYER_VERBOSE
	DEBUG_LOG_V("<<< TEST1: my_nid %d t %lu "
							"example1(from) %d example2(t) %d (good) >>>\n", 
							my_nid, request->header.ticket, 
							request->example1, request->example2);
#else
	DEBUG_LOG_V("<<< TEST1: my_nid %d example1(from) %d "
						"example2(t) %d (good) >>>\n", 
						my_nid, request->example1, request->example2);
#endif

	pcn_kmsg_free_msg(request);
	return;
}

static int handle_self_test(struct pcn_kmsg_message* inc_msg)
{
	struct test_msg_t *request = (struct test_msg_t*) inc_msg;
	DEBUG_LOG_V("%s(): message handler is called from cpu %d "
				"successfully.\n", __func__, request->header.from_nid);

	pcn_kmsg_free_msg(request);
	return 0;
}





/* tests */
/* ----- 1st testing -----
 * 	[we are here]
 *	[compose]
 *  send       ---->   irq (recv)
 *  [done]
 */
static int test1(void)
{
	int i;
	static int cnt = 0;
	for (i = 0; i < MAX_NUM_NODES; i++) {
		test_request_t *req;
		if (my_nid == i) continue;

		req = pcn_kmsg_alloc_msg(sizeof(*req));
		BUG_ON(!req);
		req->header.type = PCN_KMSG_TYPE_FIRST_TEST;
		req->header.prio = PCN_KMSG_PRIO_NORMAL;

		/* msg essentials */
		/* ------------------------------------------------------------ */
		/* msg dependences */
		req->example1 = my_nid;
		req->example2 = ++cnt;
		memset(&req->msg,'J', sizeof(req->msg));
		DEBUG_LOG_V("\n%s(): example2(t) %d strlen(req->msg) %d "
							"to all others\n", __func__, req->example2,
													(int)strlen(req->msg));

		//pcn_kmsg_send(i, (struct pcn_kmsg_message*) req, sizeof(*req));
		pcn_kmsg_send(i, req, sizeof(*req));
		pcn_kmsg_free_msg(req);
	}
	return 0;
}

/* ===== 2nd testing: r_read =====
 * 	[we are here]
 *	[compose]
 *  send       ----->   irq (recv)
 *                      perform READ
 * irq (recv)  <-----   send
 * 
 */
static int test2(void)
{
	int i;

	remote_thread_rdma_rw_t *req_rdma_read;
	req_rdma_read = kmalloc(sizeof(*req_rdma_read), GFP_KERNEL);
	if (!req_rdma_read)
		return -1;

	req_rdma_read->header.type = PCN_KMSG_TYPE_RDMA_READ_TEST_REQUEST;
	req_rdma_read->rdma_header.rmda_type_res =
								PCN_KMSG_TYPE_RDMA_READ_TEST_RESPONSE;
	req_rdma_read->header.prio = PCN_KMSG_PRIO_NORMAL;

	/* msg essentials */
	/* ------------------------------------------------------------ */
	/* msg dependences */

	/* READ/WRITE specific: *buf, size */
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
		pcn_kmsg_send_rdma(i, req_rdma_read,
							sizeof(*req_rdma_read), g_remote_read_len);
		DEBUG_LOG_V("\n\n\n");
	}
	//pcn_kmsg_free_msg(req_rdma_read);
	kfree(req_rdma_read);
	return 0;
}

/* ===== 3rd testing: r_write =====
 * 	[we are here]
 *	[compose]
 *  send       ----->   irq (recv)
 *                      perform WRITE
 * irq (recv)  <-----   send
 * 
 */
static int test3(void)
{
	int i;

	remote_thread_rdma_rw_t *req_rdma_write;
	//req_rdma_write = pcn_kmsg_alloc_msg(sizeof(*req_rdma_write));
	req_rdma_write = kmalloc(sizeof(*req_rdma_write), GFP_KERNEL);

	if (!req_rdma_write)
		return -1;

	req_rdma_write->header.type = PCN_KMSG_TYPE_RDMA_WRITE_TEST_REQUEST;
	req_rdma_write->rdma_header.rmda_type_res =
								PCN_KMSG_TYPE_RDMA_WRITE_TEST_RESPONSE;
	req_rdma_write->header.prio = PCN_KMSG_PRIO_NORMAL;

	/* msg essentials */
	/* ------------------------------------------------------------ */
	/* msg dependences */

	/* READ/WRITE specific */
	req_rdma_write->rdma_header.is_write = true;

	req_rdma_write->rdma_header.your_buf_ptr = g_test_write_buf;

	for (i = 0; i < MAX_NUM_NODES; i++) {
		if (my_nid == i) continue;
		pcn_kmsg_send_rdma(i, req_rdma_write,
							sizeof(*req_rdma_write), g_rdma_write_len);
	}
	//pcn_kmsg_free_msg(req_rdma_write);
	kfree(req_rdma_write);
	return 0;
}

static int kthread_test1(void* arg0)
{
	int i;
	DEBUG_LOG_V("%s(): created\n", __func__);
	for (i = 0; i < MAX_CONCURRENT_THREADS; i++)
		test1();
	return 0;
}

static int kthread_test2(void* arg0)
{
	int i;
	DEBUG_LOG_V("%s(): created\n", __func__);
	for (i = 0; i < MAX_CONCURRENT_THREADS; i++)
		test2();
	return 0;
}

static int kthread_test3(void* arg0)
{
	int i;
	DEBUG_LOG_V("%s(): created\n", __func__);
	for (i = 0; i < MAX_CONCURRENT_THREADS; i++)
		test3();
	return 0;
}

void test_send_throughput(unsigned long long payload_size)
{
	int i, dst = 0;
	struct timeval t1, t2;
	struct test_msg_t *msg = pcn_kmsg_alloc_msg(sizeof(*msg));

	msg->header.type = PCN_KMSG_TYPE_SELFIE_TEST;
	memset(&msg->payload, 'b', payload_size);

	if (!my_nid)
		dst = 1;

	do_gettimeofday(&t1);
	for (i = 0; i < MAX_TESTING_SIZE/payload_size; i++)
		pcn_kmsg_send(dst, msg, payload_size + sizeof(msg->header));
	do_gettimeofday(&t2);

	if (t2.tv_usec-t1.tv_usec >= 0) {
		EXP_DATA("Send one-way: send payload size %llu, "
					"total size %d, %llu times, spent %ld.%06ld s\n",
					payload_size, MAX_TESTING_SIZE,
					MAX_TESTING_SIZE / payload_size, 
					t2.tv_sec - t1.tv_sec, t2.tv_usec - t1.tv_usec);
	} else {
		EXP_DATA("Send one-way: send payload size %llu, "
					"total size %d, %llu times, spent %ld.%06ld s\n",
					payload_size, MAX_TESTING_SIZE,
					MAX_TESTING_SIZE / payload_size,
					t2.tv_sec - t1.tv_sec - 1,
					(1000000 - (t1.tv_usec - t2.tv_usec)));
	}

	pcn_kmsg_free_msg(msg);
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
static int rdma_RW_test(unsigned long long payload_size,
						unsigned long long iter, bool is_rdma_read, int t)
{
	unsigned long long i, j;
	struct timeval t1, t2;

	do_gettimeofday(&t1);
	for (j = 0; j < iter; j++) {
		for (i = 0; i < MAX_NUM_NODES; i++) {
			remote_thread_rdma_rw_t *req_rdma;
			remote_thread_rdma_rw_t *res;
			struct wait_station *ws;
			if (my_nid == i) continue;

			req_rdma = pcn_kmsg_alloc_msg(sizeof(*req_rdma));
			BUG_ON(!req_rdma);

			ws = get_wait_station(current);
			req_rdma->remote_ws = ws->id;

			if (is_rdma_read == true) {
				req_rdma->header.type = PCN_KMSG_TYPE_RDMA_READ_TEST_REQUEST;
				req_rdma->rdma_header.rmda_type_res =
									PCN_KMSG_TYPE_RDMA_READ_TEST_RESPONSE;
			} else {
				req_rdma->header.type = PCN_KMSG_TYPE_RDMA_WRITE_TEST_REQUEST;
				req_rdma->rdma_header.rmda_type_res =
									PCN_KMSG_TYPE_RDMA_WRITE_TEST_RESPONSE;
			}

			req_rdma->header.prio = PCN_KMSG_PRIO_NORMAL;
			req_rdma->t_num = t;

			/* msg essentials */
			/* ------------------------------------------------------------ */
			/* msg dependences */

			/* READ/WRITE specific */
			if (is_rdma_read == true)
				req_rdma->rdma_header.is_write = false;
			else
				req_rdma->rdma_header.is_write = true;

			req_rdma->rdma_header.your_buf_ptr = dummy_act_buf[i][t];

			pcn_kmsg_send_rdma(i, req_rdma,
							sizeof(*req_rdma), (unsigned int)payload_size);
			pcn_kmsg_free_msg(req_rdma);

			res = wait_at_station(ws);
			put_wait_station(ws);
			pcn_kmsg_free_msg(res);
		}
	}
	do_gettimeofday(&t2);

	EXP_DATA("RDMA %s: size %llu, iter %llu, %ld.%06ld\n",
										is_rdma_read ? "READ" : "WRITE",
										payload_size, iter,

										t2.tv_usec - t1.tv_usec >= 0 ?
										t2.tv_sec - t1.tv_sec :
										t2.tv_sec - t1.tv_sec-1,

										t2.tv_usec - t1.tv_usec >= 0 ?
										t2.tv_usec - t1.tv_usec :
										(1000000 - (t1.tv_usec - t2.tv_usec)));
	return 0;
}


static int kthread_rdma_RW_test(void* arg0)
{
	struct kmsg_arg* karg = arg0;
	rdma_RW_test(karg->payload_size, karg->iter, karg->isread, karg->tid);
	atomic_inc(karg->thread_done_cnt);
	wake_up_interruptible(karg->wait_thread_sem);
	kfree(arg0);
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
static int rdma_farm_test(unsigned long long payload_size,
							unsigned long long iter, int t)
{
	unsigned long long i, j;
	struct timeval t1, t2;

	do_gettimeofday(&t1);
	for (j = 0; j < iter; j++) {
		for (i = 0; i < MAX_NUM_NODES; i++) {
			remote_thread_rdma_rw_t req_rdma;
			if (my_nid == i) continue;

			req_rdma.header.type = PCN_KMSG_TYPE_RDMA_WRITE_TEST_REQUEST;
			//req_rdma->rdma_header.rmda_type_res =
			//						PCN_KMSG_TYPE_RDMA_WRITE_TEST_RESPONSE;
			req_rdma.header.prio = PCN_KMSG_PRIO_NORMAL;

			req_rdma.rdma_header.is_write = true;
			req_rdma.rdma_header.your_buf_ptr = dummy_act_buf[i][t];

			req_rdma.t_num = t;

			pcn_kmsg_send_rdma(i, &req_rdma,
							sizeof(req_rdma), (unsigned int)payload_size);
		}
	}
	do_gettimeofday(&t2);

	EXP_DATA("RDMA FaRM: size %llu, iter %llu,"
			" %ld.%06ld\n", payload_size, iter,
							t2.tv_usec - t1.tv_usec >= 0 ?
							t2.tv_sec - t1.tv_sec :
							t2.tv_sec - t1.tv_sec - 1,

							t2.tv_usec - t1.tv_usec >= 0 ?
							t2.tv_usec - t1.tv_usec :
							(1000000 - (t1.tv_usec - t2.tv_usec)));
	return 0;
}

static int kthread_rdma_farm_test(void* arg0)
{
	struct kmsg_arg* karg = arg0;
	rdma_farm_test(karg->payload_size, karg->iter, karg->tid);
	atomic_inc(karg->thread_done_cnt);
	wake_up_interruptible(karg->wait_thread_sem);
	kfree(arg0);
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
static int rdma_farm_mem_cpy_test(unsigned long long payload_size,
								unsigned long long iter, int t)
{
	unsigned long long i, j;
	struct timeval t1, t2;

	do_gettimeofday(&t1);
	for (j = 0; j < iter; j++) {
		for (i = 0; i < MAX_NUM_NODES; i++) {
			char *act_buf;
			remote_thread_rdma_rw_t *req_rdma;
			if (my_nid == i) continue;

			req_rdma = pcn_kmsg_alloc_msg(sizeof(*req_rdma));
			BUG_ON(!req_rdma);

			req_rdma->header.type = PCN_KMSG_TYPE_RDMA_WRITE_TEST_REQUEST;
			//req_rdma->rdma_header.rmda_type_res =					// Not used for FaRM WRITE
			//							PCN_KMSG_TYPE_RDMA_WRITE_TEST_RESPONSE;
			req_rdma->header.prio = PCN_KMSG_PRIO_NORMAL;

			req_rdma->rdma_header.is_write = true;
			req_rdma->rdma_header.your_buf_ptr = dummy_act_buf[i][t];

			req_rdma->t_num = t;

			act_buf = pcn_kmsg_send_rdma(i, req_rdma,
						sizeof(*req_rdma), (unsigned int)payload_size);
			if (act_buf) {
#if POPCORN_DEBUG_MSG_IB
				int lengh = 0;

				DEBUG_LOG_V("%s(): head length is 0x%.2x %.2x %.2x %.2x "
												"MUST BE %.8x(O)\n", __func__,
																*(act_buf + 0),
																*(act_buf + 1),
																*(act_buf + 2),
																*(act_buf + 3),
													(unsigned int)payload_size);
				DEBUG_LOG_V("%s(): is data 0x%.2x\n", __func__, *(act_buf + 4));
				DEBUG_LOG_V("%s(): last byte 0x%.2x\n",
										__func__, *(act_buf+payload_size-1));

				lengh += *(act_buf + 0) << 24;
				lengh += *(act_buf + 1) << 16;
				lengh += *(act_buf + 2) << 8;
				lengh += *(act_buf + 3) << 0;
				DEBUG_LOG_V("%s(): return int %d payload_size %llu\n\n",
											__func__, lengh, payload_size);
#endif
				kfree(act_buf);
			} else
				printk("%s(): recv size 0\n\n", __func__);

			pcn_kmsg_free_msg(req_rdma);
		}
	}
	do_gettimeofday(&t2);

	EXP_DATA("RDMA_POLL: size %llu, iter %llu,"
						" %ld.%06ld\n", payload_size, iter,
						t2.tv_usec - t1.tv_usec >= 0 ?
						t2.tv_sec - t1.tv_sec :
						t2.tv_sec - t1.tv_sec-1,

						t2.tv_usec - t1.tv_usec >= 0 ?
						t2.tv_usec - t1.tv_usec :
						(1000000 - (t1.tv_usec - t2.tv_usec)));
	return 0;
}

static int kthread_rdma_farm_mem_cpy_test(void* arg0)
{
	struct kmsg_arg* karg = arg0;
	rdma_farm_mem_cpy_test(karg->payload_size, karg->iter, karg->tid);
	atomic_inc(karg->thread_done_cnt);
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
static int rdma_farm2_data(unsigned long long payload_size,
							unsigned long long iter, int t)
{
	unsigned long long i, j;
	struct timeval t1, t2;

	do_gettimeofday(&t1);
	for (j = 0; j < iter; j++) {
		for (i = 0; i < MAX_NUM_NODES; i++) {
			remote_thread_rdma_rw_t *req_rdma;
			if (my_nid == i) continue;

			req_rdma = pcn_kmsg_alloc_msg(sizeof(*req_rdma));
			BUG_ON(!req_rdma);

			req_rdma->header.type = PCN_KMSG_TYPE_RDMA_WRITE_TEST_REQUEST;
			req_rdma->header.prio = PCN_KMSG_PRIO_NORMAL;

			req_rdma->rdma_header.is_write = true;
			req_rdma->rdma_header.your_buf_ptr = dummy_act_buf[i][t];

			req_rdma->t_num = t;

			pcn_kmsg_send_rdma(i, req_rdma,
						sizeof(*req_rdma), (unsigned int)payload_size);

			/* data's been done in your_buf_ptr*/

			pcn_kmsg_free_msg(req_rdma);
		}
	}
	do_gettimeofday(&t2);

	EXP_DATA("RDMA NOTIFY: size %llu, iter %llu,"
						" %ld.%06ld\n", payload_size, iter,
						t2.tv_usec - t1.tv_usec >= 0 ?
						t2.tv_sec - t1.tv_sec :
						t2.tv_sec - t1.tv_sec - 1,

						t2.tv_usec - t1.tv_usec >= 0 ?
						t2.tv_usec - t1.tv_usec :
						(1000000 - (t1.tv_usec - t2.tv_usec)));
	return 0;
}

static int kthread_rdma_farm2_data(void* arg0)
{
	struct kmsg_arg* karg = arg0;
	rdma_farm2_data(karg->payload_size, karg->iter, karg->tid);
	atomic_inc(karg->thread_done_cnt);
	wake_up_interruptible(karg->wait_thread_sem);
	kfree(arg0);
	return 0;
}

/*	mimic_read:
 *			=====>
 *					[copy]
 *			<-----
 */
void test_send_read_throughput(unsigned long long payload_size,
								 unsigned long long iter, int tid)
{
	int i, dst;
	struct timeval t1, t2;

	do_gettimeofday(&t1);
	for (i = 0; i < iter; i++) {
		for (dst = 0; dst < MAX_NUM_NODES; dst++) {
			struct wait_station *ws;
			struct mimic_rw_msg_request *req;
			struct mimic_rw_signal_request *res;
			if (my_nid == dst) continue;
			req = __alloc_send_roundtrip_read_request();
			BUG_ON(!req);

			ws = get_wait_station(current);
			req->remote_ws = ws->id;
			req->size = payload_size;
			req->tid = tid;
			memcpy(&req->payload, dummy_send_buf[dst][tid], payload_size);

			DEBUG_CORRECTNESS("%s(): r local to remote size %llu = "
							"sizeof(*req)(%lu) - sizeof(req->payload)(%lu)"
							" + payload_size(%llu)\n", __func__,
							sizeof(*req) - sizeof(req->payload) + payload_size,
							sizeof(*req), sizeof(req->payload), payload_size);

			pcn_kmsg_send(dst, req,
						sizeof(*req) - sizeof(req->payload) + payload_size);

			pcn_kmsg_free_msg(req);
			res = wait_at_station(ws);
			put_wait_station(ws);

			pcn_kmsg_free_msg(res);
		}
	}
	do_gettimeofday(&t2);

	_show_time(&t1, &t2, payload_size, iter, "Send roundtrip (mimic READ)");
}

/*	mimic_write:	(more often)
 *			----->
 *			<====
 *	 [copy]
 *
 */
void test_send_write_throughput(unsigned long long payload_size,
								unsigned long long iter, int tid)
{
	int i, dst;
	struct timeval t1, t2;

	do_gettimeofday(&t1);
	for (i = 0; i < iter; i++) {
		for (dst = 0; dst < MAX_NUM_NODES; dst++) {
			struct wait_station *ws;
			struct mimic_rw_signal_request *req;
			struct mimic_rw_msg_request *res;
			if (my_nid == dst) continue;
			req = __alloc_send_roundtrip_write_request();
			BUG_ON(!req);

			ws = get_wait_station(current);
			req->remote_ws = ws->id;
			req->size = payload_size;
			req->tid = tid;

			pcn_kmsg_send(dst, req, sizeof(*req));

			pcn_kmsg_free_msg(req);
			res = wait_at_station(ws);
			put_wait_station(ws);

			//printk("mimic WRITE\n");
			memcpy(dummy_send_buf[dst][tid], &res->payload, payload_size);

			pcn_kmsg_free_msg(res);
		}
	}
	do_gettimeofday(&t2);
	_show_time(&t1, &t2, payload_size, iter, "Recv roundtrip (mimic WRITE)");
}

static int kthread_test_send_rw_throughput(void* arg0)
{
	struct kmsg_arg* karg = arg0;
	if (karg->isread)
		test_send_read_throughput(karg->payload_size, karg->iter, karg->tid);
	else
		test_send_write_throughput(karg->payload_size, karg->iter, karg->tid);
	atomic_inc(karg->thread_done_cnt);
	wake_up_interruptible(karg->wait_thread_sem);
	kfree(arg0);
	return 0;
}

static int rdma_RW_inv_test(void* buf, unsigned long long payload_size,
							bool is_rdma_read, int t)
{
	unsigned long long i;

	remote_thread_rdma_rw_t *req_rdma;
	req_rdma = pcn_kmsg_alloc_msg(sizeof(*req_rdma));
	if (!req_rdma)
		return -1;

	if (is_rdma_read == true) {
		req_rdma->header.type = PCN_KMSG_TYPE_RDMA_READ_TEST_REQUEST;
		req_rdma->rdma_header.rmda_type_res =
								PCN_KMSG_TYPE_RDMA_READ_TEST_RESPONSE;
	} else {
		req_rdma->header.type = PCN_KMSG_TYPE_RDMA_WRITE_TEST_REQUEST;
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
		pcn_kmsg_send_rdma(i, req_rdma, sizeof(*req_rdma),
							(unsigned int)payload_size);
		DEBUG_LOG_V("\n\n\n");
	}
	msleep(1000); // wait for taking effect

	pcn_kmsg_free_msg(req_rdma);
	return 0;
}

static ssize_t write_proc(struct file * file, 
							const char __user * buffer,
							size_t count, loff_t *ppos)
{
	int i, cnt = 0;
	struct task_struct *t;
	char *cmd, *argv[MAX_ARGS_NUM];

	/* For thread_join */
	atomic_t thread_done_cnt;
	wait_queue_head_t wait_thread_sem;

	int args = 0;
	char *tok, *end;
	struct kmsg_arg karg;

	thread_done_cnt.counter = 0;
	init_waitqueue_head(&wait_thread_sem);

	if (!try_module_get(THIS_MODULE))
		return -ENODEV;

	cmd = kmalloc(count, GFP_KERNEL);
	if (!cmd) {
		printk(KERN_ERR "kmalloc failure\n");
		return -ENOMEM;
	}

	if (copy_from_user(cmd, buffer, count)) {
		return -EFAULT;
	}

	/* remove the \n */
	cmd[count - 1] = 0;

	/* start to parse */
	for (i = 0; i < MAX_ARGS_NUM; i++) {
		argv[i] = NULL;
	}
	
	tok = cmd; end = cmd;
	while (tok) {
		argv[args] = strsep(&end, " ");
		KRPRINT_INIT("%s\n", tok);
		args++;
		tok = end;
	}
	
	for (i = 0; i < MAX_ARGS_NUM; i++) {
		if (argv[i])
			KRPRINT_INIT("argv[%d] = %s\n", i, argv[i]);
	}

#ifdef CONFIG_POPCORN_STAT
	printk(KERN_WARNING "You are collecting statistics "
			"and may get inaccurate performance data now\n");
#endif
	
	KRPRINT_INIT("\n\n[ proc write |%s| cnt %ld ] [%d args] \n", 
												cmd, count, args);

	if (args >= 2)
		karg.payload_size = simple_strtoull(argv[1], NULL, 0);
	if (args >= 3)
		karg.iter = simple_strtoull(argv[2], NULL, 0);

	/* do the coresponding work */
	if (cmd[0] == '0') {
		printk("Reserved. Not implemented yet\n");
	}
	else if (cmd[0] == '1' && cmd[1] == '\0') {
		struct timeval t1;
		struct timeval t2;
		
		do_gettimeofday(&t1);
		while (++cnt <= TEST1_MSG_COUNT)
			test1();
		do_gettimeofday(&t2);
		if ( t2.tv_usec-t1.tv_usec >= 0) {
			EXP_DATA("Send throughput result: send msg size %d, "
						"total size %d, %d times, spent %ld.%06ld s\n",
						TEST1_PAYLOAD_SIZE, MAX_TESTING_SIZE,
						TEST1_MSG_COUNT, t2.tv_sec - t1.tv_sec,
						t2.tv_usec - t1.tv_usec);
		} else {
			EXP_DATA("Send throughput result: send msg size %d, "
						"total size %d, %d times, spent %ld.%06ld s\n",
						TEST1_PAYLOAD_SIZE, MAX_TESTING_SIZE,
						TEST1_MSG_COUNT, t2.tv_sec - t1.tv_sec - 1,
						(1000000 - (t1.tv_usec - t2.tv_usec)));
		}
		EXP_LOG("test%c done\n\n\n\n", cmd[0]);
	}
	else if (cmd[0] == '2' && cmd[1] == '\0') {
		setup_read_buf();
		while (++cnt <= 5000)
			test2();
		EXP_LOG("test%c done\n\n\n\n", cmd[0]);
	}
	else if (cmd[0] == '3' && cmd[1] == '\0') {
		setup_write_buf();
		while (++cnt <= 5000)
			test3();
		EXP_LOG("test%c done\n\n\n\n", cmd[0]);
	}
	else if (cmd[0] == '4' && cmd[1] == '\0') {
		for (i = 0; i < 10; i++) {
			t = kthread_run(kthread_test1, NULL, "kthread_test1()");
			BUG_ON(IS_ERR(t));
		}
		EXP_LOG("test%c done\n\n\n\n", cmd[0]);
	}
	else if (cmd[0] == '5' && cmd[1] == '\0') {
		setup_read_buf();
		for (i = 0; i < 10; i++) {
			t = kthread_run(kthread_test2, NULL, "kthread_test2()");
			BUG_ON(IS_ERR(t));
		}
		EXP_LOG("test%c done\n\n\n\n", cmd[0]);
	}
	else if (cmd[0] == '6' && cmd[1] == '\0') {
		setup_write_buf();
		for (i = 0; i < 10; i++) {
			t = kthread_run(kthread_test3, NULL, "kthread_test3()");
			BUG_ON(IS_ERR(t));
		}
		EXP_LOG("test%c done\n\n\n\n", cmd[0]);
	}
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
		EXP_LOG("test%c done\n\n\n\n", cmd[0]);
	}
	else if (!memcmp(argv[0], "10", 2)) {
		if (!cmp_args(args, 2)) {
			show_instruction();
			goto exit;
		}
		for (i = 0; i < ITER; i++) {
			test_send_throughput(karg.payload_size);
		}
		EXP_LOG("test %c%c test_send_throughput() done\n\n\n", cmd[0], cmd[1]);
	}
	else if (!memcmp(argv[0], "11", 2) || !memcmp(argv[0], "12", 2)) {
		if (cmp_args(4, args)) {
			karg.isread = !memcmp(argv[0], "11", 2) ? true : false;
			for (i = 0; i < MAX_CONCURRENT_THREADS; i++) {
				struct kmsg_arg *_karg;
				_karg = kmalloc(sizeof(*_karg), GFP_KERNEL);
				_karg->payload_size = karg.payload_size;
				_karg->iter = karg.iter;
				_karg->tid = i;
				_karg->isread = karg.isread;
				_karg->wait_thread_sem = &wait_thread_sem;
				_karg->thread_done_cnt = &thread_done_cnt;
				t = kthread_run(kthread_test_send_rw_throughput, _karg,
								"kthread_test_send_rw_throughput()");
				BUG_ON(IS_ERR(t));
			}
			wait_event_interruptible(wait_thread_sem,
				atomic_read(&thread_done_cnt) >= MAX_CONCURRENT_THREADS);
			EXP_LOG("test %s %s (M); test_send_roundtrip_throughput() done\n",
					argv[0], !memcmp(argv[0], "11", 2) ?
					"Mimic READ" : "Mimic WRITE");
		} else if (cmp_args(3, args)) {
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
			show_instruction();
			goto exit;
		}
	}
	else if (!memcmp(argv[0], "13", 2) || !memcmp(argv[0], "14", 2)) {
		if (cmp_args(4, args)) {
			karg.isread = !memcmp(argv[0], "13", 2) ? true : false;
			for (i = 0; i < MAX_CONCURRENT_THREADS; i++) {
				struct kmsg_arg *_karg;
				_karg = kmalloc(sizeof(*_karg), GFP_KERNEL);
				_karg->payload_size = karg.payload_size;
				_karg->iter = karg.iter;
				_karg->tid = i;
				_karg->isread = karg.isread;
				_karg->wait_thread_sem = &wait_thread_sem;
				_karg->thread_done_cnt = &thread_done_cnt;
				t = kthread_run(kthread_rdma_RW_test, _karg,
								"kthread_rdma_RW_test()");
				BUG_ON(IS_ERR(t));
			}
			wait_event_interruptible(wait_thread_sem,
				atomic_read(&thread_done_cnt) >= MAX_CONCURRENT_THREADS);
			EXP_LOG("RDMA %s (M): rdma_RW_test() done\n\n\n",
					!memcmp(argv[0], "13", 2) ? "READ" : "WRITE");
		} else if (cmp_args(3, args)) {
			rdma_RW_test(karg.payload_size, karg.iter,
						!memcmp(argv[0], "13", 2) ? true : false, 0);
			EXP_LOG("RDMA %s rdma_RW_test() single thread done\n\n\n",
					!memcmp(argv[0], "13", 2) ? "READ" : "WRITE");
		} else {
			show_instruction();
			goto exit;
		}
	}
	else if (!memcmp(argv[0], "15", 2)) {
		/* Because of this LENGTH is not divided by 10,
		 * the last few chars are not changed!			*/
		int z, ofs = MAX_MSG_LENGTH/10;

		printk("----------------------------------\n");
		printk("----- READ test sanity start -----\n");
		printk("----------------------------------\n\n");

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

		printk("---------------------------------\n");
		printk("----- READ test sanity done -----\n");
		printk("---------------------------------\n\n");
		msleep(5000);
		init_RW_dummy_buf(0);
		show_RW_dummy_buf();
		msleep(5000);

		printk("----------------------------------\n");
		printk("----- WRITE test sanity start ----\n");
		printk("----------------------------------\n\n");

		for (z = 0; z < 10; z++) {
			rdma_RW_inv_test(dummy_act_buf[0][0]+(z*ofs), ofs, 0, 0);
			show_RW_dummy_buf();
			msleep(3000);
		}
		printk("----------------------------------\n");
		printk("----- WRITE test sanity done -----\n");
		printk("----------------------------------\n\n");
	} else if (!memcmp(argv[0], "16", 2)) {
		init_RW_dummy_buf(0);
	} else if (!memcmp(argv[0], "17", 2)) {
		show_RW_dummy_buf();
	} else if (!memcmp(argv[0], "20", 2)) {
		if (cmp_args(4, args)) {
			int k;
			for (k = 0; k < MAX_CONCURRENT_THREADS; k++) {
				struct kmsg_arg *_karg;
				_karg = kmalloc(sizeof(*_karg), GFP_KERNEL);
				_karg->payload_size = karg.payload_size;
				_karg->iter = karg.iter;
				_karg->tid = k;
				_karg->isread = karg.isread;
				_karg->wait_thread_sem = &wait_thread_sem;
				_karg->thread_done_cnt = &thread_done_cnt;
				t = kthread_run(kthread_rdma_farm_test, _karg,
								"kthread_rdma_farm_test()");
				//smp_mb();
				//kfree(_karg);
				BUG_ON(IS_ERR(t));
			}
			wait_event_interruptible(wait_thread_sem,
				atomic_read(&thread_done_cnt) >= MAX_CONCURRENT_THREADS);
			EXP_LOG("RDMA %c (M): FaRM RDMA WRITE "
					"rdma_farm_test() done\n", cmd[0]);
		} else if (cmp_args(3, args)) {
			rdma_farm_test(karg.payload_size, karg.iter, 0);
			EXP_LOG("RDMA %c: FaRM RDMA WRITE "
					"rdma_farm_test() done\n", cmd[0]);
		} else {
			show_instruction();
			goto exit;
		}
	} else if (!memcmp(argv[0], "21", 2)) {
		if (cmp_args(4, args)) {
			for (i = 0; i < MAX_CONCURRENT_THREADS; i++) {
				struct kmsg_arg *_karg;
				_karg = kmalloc(sizeof(*_karg), GFP_KERNEL);
				_karg->payload_size = karg.payload_size;
				_karg->iter = karg.iter;
				_karg->tid = i;
				_karg->isread = karg.isread;
				_karg->wait_thread_sem = &wait_thread_sem;
				_karg->thread_done_cnt = &thread_done_cnt;
				t = kthread_run(kthread_rdma_farm_mem_cpy_test, _karg,
								"kthread_rdma_farm_mem_cpy_test()");
				//smp_mb();
				//kfree(_karg);
				BUG_ON(IS_ERR(t));
			}
			wait_event_interruptible(wait_thread_sem,
				atomic_read(&thread_done_cnt) >= MAX_CONCURRENT_THREADS);
			EXP_LOG("RDMA %c (M): FaRM RDMA WRITE "
					"rdma_farm_mem_cpy_test() done\n", cmd[0]);
		} else if (cmp_args(3, args)) {
			rdma_farm_mem_cpy_test(karg.payload_size, karg.iter, 0);
			EXP_LOG("RDMA %c: FaRM RDMA WRITE "
					"rdma_farm_mem_cpy_test() done\n", cmd[0]);
		} else {
			show_instruction();
			goto exit;
		}
	} else if (!memcmp(argv[0], "22", 2)) {
		if (cmp_args(4, args)) {
			for (i = 0; i < MAX_CONCURRENT_THREADS; i++) {
				struct kmsg_arg *_karg;
				_karg = kmalloc(sizeof(*_karg), GFP_KERNEL);
				_karg->payload_size = karg.payload_size;
				_karg->iter = karg.iter;
				_karg->tid = i;
				_karg->isread = karg.isread;
				_karg->wait_thread_sem = &wait_thread_sem;
				_karg->thread_done_cnt = &thread_done_cnt;
				t = kthread_run(kthread_rdma_farm2_data, _karg,
								"kthread_rdma_farm2_data()");
				//smp_mb();
				//kfree(_karg);
				BUG_ON(IS_ERR(t));
			}
			wait_event_interruptible(wait_thread_sem,
				atomic_read(&thread_done_cnt) >= MAX_CONCURRENT_THREADS);
			EXP_LOG("RDMA %c (M): FaRM RDMA WRITE "
					"rdma_farm2_data() done\n", cmd[0]);
		} else if (cmp_args(3, args)) {
			rdma_farm2_data(karg.payload_size, karg.iter, 0);
			EXP_LOG("RDMA %c: FaRM RDMA WRITE "
					"rdma_farm2_data() done\n", cmd[0]);
		} else {
			show_instruction();
			goto exit;
		}
	} else {
		printk("Not support yet. Try \"1,2,3,4,5,6,9,10\"\n");
	}

exit:
	kfree(cmd);
	module_put(THIS_MODULE);
	KRPRINT_INIT("proc write done!!\n");
	return count;	// if not reach count, will reenter again
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
	.write = write_proc,
};


/*
 *	Too large to static allocate - if do statically, array drain happens
 */
static void __reply_send_r_roundtrip(struct mimic_rw_msg_request *req, int ret)
{
	struct mimic_rw_signal_request *res = pcn_kmsg_alloc_msg(sizeof(*res));
	res->header.type = PCN_KMSG_TYPE_SEND_ROUND_READ_RESPONSE;
	res->header.prio = PCN_KMSG_PRIO_NORMAL;
	res->remote_ws = req->remote_ws;
	//res->size = req->size;
	pcn_kmsg_send(req->header.from_nid, res, sizeof(*res));
	pcn_kmsg_free_msg(res);
}

static void process_send_roundtrip_r_request(struct work_struct *_work)
{
	struct pcn_kmsg_work *work = (struct pcn_kmsg_work *)_work;
	struct mimic_rw_msg_request *req = work->msg;

	/* mimic READ */
	memcpy(dummy_send_buf[req->header.from_nid][req->tid],
			&req->payload, req->size);

	__reply_send_r_roundtrip(req, -EINVAL);

	pcn_kmsg_free_msg(work->msg);
	kfree(work);
}

static void process_send_roundtrip_r_response(struct work_struct *_work)
{
	struct pcn_kmsg_work *work = (struct pcn_kmsg_work *)_work;
	struct mimic_rw_signal_request *res = work->msg;
	struct wait_station *ws = wait_station(res->remote_ws);

	ws->private = res;
	smp_mb();
	complete(&ws->pendings);

	kfree(work);
}

/**
 *	Too large to static allocate - if do statically, array drain happens
 **/
static void __reply_send_w_roundtrip(struct mimic_rw_signal_request *req,
									int ret)
{
	struct mimic_rw_msg_request *res = pcn_kmsg_alloc_msg(sizeof(*res));
	res->header.type = PCN_KMSG_TYPE_SEND_ROUND_WRITE_RESPONSE;
	res->header.prio = PCN_KMSG_PRIO_NORMAL;
	res->remote_ws = req->remote_ws;
	res->size = req->size;

	DEBUG_CORRECTNESS("%s(): w remote back size %lu = "
				"sizeof(*res)%lu - sizeof(res->payload)%lu + req->size %d\n",
				__func__, sizeof(*res) - sizeof(res->payload) + req->size,
							sizeof(*res), sizeof(res->payload), req->size);

	/* mimic WRITE */
	memcpy(&res->payload,
			dummy_send_buf[req->header.from_nid][req->tid],
			req->size);
	pcn_kmsg_send(req->header.from_nid, res,
					sizeof(*res) - sizeof(res->payload) + req->size);
	pcn_kmsg_free_msg(res);
}

static void process_send_roundtrip_w_request(struct work_struct *_work)
{
	struct pcn_kmsg_work *work = (struct pcn_kmsg_work *)_work;
	struct mimic_rw_signal_request *req = work->msg;

	__reply_send_w_roundtrip(req, -EINVAL);

	pcn_kmsg_free_msg(req);
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

	kfree(work);
}


// READ
static void process_handle_test_read_request(struct work_struct *_work)
{
	struct pcn_kmsg_work *work = (struct pcn_kmsg_work *)_work;
	remote_thread_rdma_rw_t *req = work->msg;

	/* Prepare your *paddr */
	void* paddr = dummy_pass_buf[req->header.from_nid][req->t_num];

	/* RDMA routine */
	pcn_kmsg_handle_remote_rdma_request(req, paddr);

	pcn_kmsg_free_msg(req);
	kfree(work);
}

static void process_handle_test_read_response(struct work_struct *_work)
{
	struct pcn_kmsg_work *work = (struct pcn_kmsg_work *)_work;
	remote_thread_rdma_rw_t *res = work->msg;
	struct wait_station *ws = wait_station(res->remote_ws);

	/* RDMA routine */
	pcn_kmsg_handle_remote_rdma_request(res, NULL);

	ws->private = res;
	smp_mb();
	complete(&ws->pendings);

	kfree(work);
}

// WRITE
static void process_handle_test_write_request(struct work_struct *_work)
{
	struct pcn_kmsg_work *work = (struct pcn_kmsg_work *)_work;
	remote_thread_rdma_rw_t *req = work->msg;

	/* Prepare your *paddr */
	void *paddr = dummy_pass_buf[req->header.from_nid][req->t_num];

	/* RDMA routine */
	pcn_kmsg_handle_remote_rdma_request(req, paddr);

	pcn_kmsg_free_msg(req);
	kfree(work);
}

static void process_handle_test_write_response(struct work_struct *_work)
{
	struct pcn_kmsg_work *work = (struct pcn_kmsg_work *)_work;
	remote_thread_rdma_rw_t *res = work->msg;
	struct wait_station *ws = wait_station(res->remote_ws);

	/* RDMA routine */
	pcn_kmsg_handle_remote_rdma_request(res, NULL);

	ws->private = res;
	smp_mb();
	complete(&ws->pendings);

	kfree(work);
}


DEFINE_KMSG_WQ_HANDLER(send_roundtrip_r_request);
DEFINE_KMSG_WQ_HANDLER(send_roundtrip_r_response);

DEFINE_KMSG_WQ_HANDLER(send_roundtrip_w_request);
DEFINE_KMSG_WQ_HANDLER(send_roundtrip_w_response);

DEFINE_KMSG_WQ_HANDLER(handle_test_read_request);
DEFINE_KMSG_WQ_HANDLER(handle_test_read_response);

DEFINE_KMSG_WQ_HANDLER(handle_test_write_request);
DEFINE_KMSG_WQ_HANDLER(handle_test_write_response);

/* example - main usage */
static int __init msg_test_init(void)
{
	int i, j;
	static struct proc_dir_entry *kmsg_test_proc;

	/* register a proc */
	printk("--- Popcorn messaging self testing proc init ---\n");
	kmsg_test_proc = proc_create("kmsg_test", 0666, NULL, &kmsg_test_ops);
	if (!kmsg_test_proc) {
		printk(KERN_ERR "cannot create /proc/kmsg_test\n");
		return -ENOMEM;
	}

	/* init dummy buffers for geting experimental data */
	if (MAX_MSG_LENGTH > PCN_KMSG_LONG_PAYLOAD_SIZE) {
		printk(KERN_ERR "MAX_MSG_LENGTH %d shouldn't be larger than "
						"PCN_KMSG_LONG_PAYLOAD_SIZE %d\n",
						MAX_MSG_LENGTH, PCN_KMSG_LONG_PAYLOAD_SIZE);
		BUG();
	}
	for (j = 0; j < MAX_NUM_NODES; j++) {
		for (i = 0; i < MAX_CONCURRENT_THREADS; i++) {
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
	/* register callback. also define in <linux/pcn_kmsg.h>  */
	pcn_kmsg_register_callback((enum pcn_kmsg_type)PCN_KMSG_TYPE_FIRST_TEST,
					(pcn_kmsg_cbftn)handle_remote_thread_first_test_request);

	/* for experimental data - send throughput */
	pcn_kmsg_register_callback(PCN_KMSG_TYPE_SELFIE_TEST, handle_self_test);
	pcn_kmsg_register_callback(PCN_KMSG_TYPE_SHOW_REMOTE_TEST_BUF,
								(pcn_kmsg_cbftn)handle_show_RW_dummy_buf);

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
	show_instruction();
	smp_mb();
	return 0;
}

static void __exit msg_test_exit(void) 
{
	int i, j;
	printk("\n\n--- Popcorn messaging self testing unloaded! ---\n\n");

	for (j = 0; j < MAX_NUM_NODES; j++) {
		for (i = 0; i < MAX_CONCURRENT_THREADS; i++) {
			kfree(dummy_act_buf[j][i]);
			kfree(dummy_pass_buf[j][i]);
		}
	}
	if (g_test_buf)
		kfree(g_test_buf);

	if (g_test_write_buf)
		kfree(g_test_write_buf);

	remove_proc_entry("kmsg_test", NULL);
}

module_init(msg_test_init);
module_exit(msg_test_exit);
MODULE_LICENSE("GPL");
