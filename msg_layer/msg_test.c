/*
 * Copyright (C) 2017 JackChuang <horenc@vt.edu>
 * 
 * Testing msg_layer    - general send recv
 *                      - large data (>=8K)
 *                      - (rdma read/write)
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

#define MAX_MSG_LENGTH 65536	// max msg payload size supported by msg_test.c

/* testing args */
#define ITER 1					// iter for test10 (send throughput)
#define MAX_ARGS_NUM 10	
#define TEST1_MSG_COUNT 100000	// specifically for test1 iterations

#define MAX_TESTING_SIZE 120*1024*1024
#define TEST1_PAYLOAD_SIZE 1024

/* proc output args */
#define PROC_BUF_SIZE 1024*1024 // proc output size

char *dummy_send_buf;				// dummy_send_buf for testing
EXPORT_SYMBOL(dummy_send_buf);

extern char *dummy_act_buf;
extern char *dummy_pass_buf;

/* for testing rdma read */
int g_remote_read_len = 8*1024; // testing size for RDMA, mimicing user buf size
int g_rdma_write_len = 8*1024;		// size wanna remote to perform write
char *g_test_buf = NULL;			// mimicing user buf
char *g_test_write_buf = NULL;		// mimicing user buf

/* workqueue arg for testing */
typedef struct {
	struct work_struct work;
	struct pcn_kmsg_message *lmsg;
} pcn_kmsg_work_t;


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
	/* you define */
	int example1;
	int example2;
#ifdef CONFIG_POPCORN_DEBUG_MSG_LAYER_VERBOSE
#endif  
	char msg[TEST1_PAYLOAD_SIZE];
}__attribute__((packed)) remote_thread_first_test_request_t;

struct test_msg_t {
	struct pcn_kmsg_hdr header;
	unsigned char payload[MAX_MSG_LENGTH];
};

struct test_msg_request_t {
	struct pcn_kmsg_hdr header;
	int remote_ws;
	unsigned int size;
	unsigned char payload[MAX_MSG_LENGTH];
};

struct test_msg_signal_t {
	struct pcn_kmsg_hdr header;
	int remote_ws;
	unsigned int size;
};

struct test_msg_response_t {
	struct pcn_kmsg_hdr header;
	int remote_ws;
};


/* testing utility */
/*
 * in the reality, this function should be called between send()s
 */
void setup_read_buf(void)
{
	// read specific (data you wanna let remote side to read)
	g_test_buf = kmalloc(g_remote_read_len, GFP_KERNEL);
	if (!g_test_buf) {
		printk(KERN_ERR "kmalloc failure\n");
		BUG();
	}

	memset(g_test_buf, 'R', g_remote_read_len);
							// user data buffer ( will be copied to rdma buf)
}

void setup_write_buf(void)
{
	// read specific (data you wanna let remote side to write)
	g_test_write_buf = kmalloc(g_rdma_write_len, GFP_KERNEL);
	if (!g_test_write_buf) {
		printk(KERN_ERR "kmalloc failure\n");
		BUG();
	}
	memset(g_test_write_buf, 'W', g_rdma_write_len);
							// user data buffer ( will be copied to rdma buf)
}

void _show_RW_dummy_buf(void)
{
	printk("<<<<< CHECK active buffer >>>>> \n"
					"_cb->rw_act_buf(first10) \"%.10s\"\n"
					"_cb->rw_act_buf(last 10) \"%.10s\"\n\n\n",
					dummy_act_buf,
					dummy_act_buf+(MAX_MSG_LENGTH-11));
	printk("<<<<< CHECK pass buffer>>>>> \n"
					"_cb->rw_pass_buf(first10) \"%.10s\"\n"
					"_cb->rw_pass_buf(last 10) \"%.10s\"\n\n\n",
					dummy_pass_buf,
					dummy_pass_buf+(MAX_MSG_LENGTH-11));
}

void show_RW_dummy_buf(void)
{
	int i;
	struct pcn_kmsg_message *request; // youTODO: make your own struct

	_show_RW_dummy_buf();

	/* send to remote a request of  showing R/W buffers */
	request = pcn_kmsg_alloc_msg(sizeof(*request));
	if (!request) BUG();

	request->header.type = PCN_KMSG_TYPE_SHOW_REMOTE_TEST_BUF;
	request->header.prio = PCN_KMSG_PRIO_NORMAL;

	for(i=0; i<MAX_NUM_NODES; i++) {
		if (my_nid==i) continue;
		pcn_kmsg_send(i, request, sizeof(*request));
	}

	pcn_kmsg_free_msg(request);
}

static void handle_show_RW_dummy_buf( struct pcn_kmsg_message *inc_lmsg)
{
	_show_RW_dummy_buf();
	pcn_kmsg_free_msg(inc_lmsg);
}

void init_RW_dummy_buf(void) {
	printk("---------------------------\n");
	printk("----- init dummy buff -----\n");
	printk("---------------------------\n");
	memset(dummy_act_buf, 'A', 10);
	memset(dummy_act_buf+10, 'B', MAX_MSG_LENGTH-10);
	memset(dummy_pass_buf, 'P', 10);
	memset(dummy_pass_buf+10, 'Q', MAX_MSG_LENGTH-10);
}

static struct test_msg_request_t *__alloc_send_roundtrip_read_request(void)
{
	struct test_msg_request_t *req = pcn_kmsg_alloc_msg(sizeof(*req));
	if (!req)
		return NULL;

	req->header.type = PCN_KMSG_TYPE_SEND_ROUND_REQUEST;
	req->header.prio = PCN_KMSG_PRIO_NORMAL;
	return req;
}


static struct test_msg_signal_t *__alloc_send_roundtrip_write_request(void)
{
	struct test_msg_signal_t *req =
						pcn_kmsg_alloc_msg(sizeof(struct test_msg_signal_t));
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
	EXP_DATA("%s: payload size %llu, %llu times, spent %ld.%06ld s\n",
						str, payload_size, iter,

						t2->tv_usec-t1->tv_usec >= 0?
						t2->tv_sec-t1->tv_sec:
						t2->tv_sec-t1->tv_sec-1,

						t2->tv_usec-t1->tv_usec >= 0?
						t2->tv_usec-t1->tv_usec:
						(1000000-(t1->tv_usec-t2->tv_usec)));
}




/* example - handler */
static void handle_remote_thread_first_test_request(
									struct pcn_kmsg_message* inc_lmsg)
{
	remote_thread_first_test_request_t* request = 
						(remote_thread_first_test_request_t*) inc_lmsg;

#ifdef CONFIG_POPCORN_DEBUG_MSG_LAYER_VERBOSE
	DEBUG_LOG_V("<<< TEST1: my_nid=%d t %lu "
							"example1(from)=%d example2(t)=%d (good) >>>\n", 
							my_nid, request->header.ticket, 
							request->example1, request->example2);
#else
	DEBUG_LOG_V("<<< TEST1: my_nid=%d example1(from)=%d "
						"example2(t)=%d (good) >>>\n", 
						my_nid, request->example1, request->example2);
#endif

	pcn_kmsg_free_msg(request);
	return;
}

static int handle_self_test(struct pcn_kmsg_message* inc_msg)
{
	struct test_msg_t *request = (struct test_msg_t*) inc_msg;
	DEBUG_LOG_V("%s(): message handler is called from cpu %d successfully.\n",
		__func__, request->header.from_nid);

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
	remote_thread_first_test_request_t* request =
										pcn_kmsg_alloc_msg(sizeof(*request));
	if (request==NULL)
		return -1;

	request->header.type = PCN_KMSG_TYPE_FIRST_TEST;
	request->header.prio = PCN_KMSG_PRIO_NORMAL;

	/* msg essentials */
	/* ------------------------------------------------------------ */
	/* msg dependences */
	request->example1 = my_nid;
	request->example2 = ++cnt;
	memset(request->msg,'J', sizeof(request->msg));
	DEBUG_LOG_V("\n%s(): example2(t) %d strlen(request->msg) %d "
						"to all others\n", __func__, request->example2,
												(int)strlen(request->msg));

	for(i=0; i<MAX_NUM_NODES; i++) {
		if (my_nid==i)
			continue;
		pcn_kmsg_send(i, (struct pcn_kmsg_message*) request,
														sizeof(*request));
	}

	pcn_kmsg_free_msg(request);
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
	volatile int i;

	remote_thread_rdma_rw_t* request_rdma_read;
	//request_rdma_read = pcn_kmsg_alloc_msg(sizeof(*request_rdma_read));
	request_rdma_read = kmalloc(sizeof(*request_rdma_read), GFP_KERNEL);
	if (request_rdma_read==NULL)
		return -1;

	request_rdma_read->header.type = PCN_KMSG_TYPE_RDMA_READ_TEST_REQUEST;
	request_rdma_read->rdma_header.rmda_type_res =
									PCN_KMSG_TYPE_RDMA_READ_TEST_RESPONSE;
	request_rdma_read->header.prio = PCN_KMSG_PRIO_NORMAL;

	/* msg essentials */
	/* ------------------------------------------------------------ */
	/* msg dependences */

	/* READ/WRITE specific: *buf, size */
	request_rdma_read->rdma_header.is_write = false;
	
	/* g_test_buf is allocated by setup_read_buf() */
	request_rdma_read->rdma_header.your_buf_ptr = g_test_buf;
	/*
	 * your buf will be copied to rdma buf for a passive remote read
	 * user should protect
	 * local buffer size for passive remote to read
	 */
	for(i=0; i<MAX_NUM_NODES; i++) {
		if (my_nid==i)
			continue;
		
		pcn_kmsg_send_rdma(i, request_rdma_read,
							sizeof(*request_rdma_read), g_remote_read_len);
		DEBUG_LOG_V("\n\n\n");
	}
	//pcn_kmsg_free_msg(request_rdma_read);
	kfree(request_rdma_read);
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

	remote_thread_rdma_rw_t* request_rdma_write;
	//request_rdma_write = pcn_kmsg_alloc_msg(sizeof(*request_rdma_write));
	request_rdma_write = kmalloc(sizeof(*request_rdma_write), GFP_KERNEL);

	if (request_rdma_write==NULL)
		return -1;

	request_rdma_write->header.type = PCN_KMSG_TYPE_RDMA_WRITE_TEST_REQUEST;
	request_rdma_write->rdma_header.rmda_type_res =
									PCN_KMSG_TYPE_RDMA_WRITE_TEST_RESPONSE;
	request_rdma_write->header.prio = PCN_KMSG_PRIO_NORMAL;

	/* msg essentials */
	/* ------------------------------------------------------------ */
	/* msg dependences */

	/* READ/WRITE specific */
	request_rdma_write->rdma_header.is_write = true;

	request_rdma_write->rdma_header.your_buf_ptr = g_test_write_buf;

	for(i=0; i<MAX_NUM_NODES; i++) {
		if (my_nid==i)
			continue;
		
		pcn_kmsg_send_rdma(i, request_rdma_write,
							sizeof(*request_rdma_write), g_rdma_write_len);
		
		DEBUG_LOG_V("\n\n\n");
	}
	//pcn_kmsg_free_msg(request_rdma_write);
	kfree(request_rdma_write);
	return 0;
}

static int kthread_test1(void* arg0)
{
	volatile int i;
	DEBUG_LOG_V("%s(): created\n", __func__);
	for(i=0; i<1000; i++)
		test1();
	return 0;
}

static int kthread_test2(void* arg0)
{
	volatile int i;
	DEBUG_LOG_V("%s(): created\n", __func__);
	for(i=0; i<300; i++)
		test2();
	return 0;
}

static int kthread_test3(void* arg0)
{
	volatile int i;
	DEBUG_LOG_V("%s(): created\n", __func__);
	for(i=0; i<300; i++)
		test3();
	return 0;
}

void test_send_throughput(unsigned long long payload_size)
{
	int i, dst = 0;
	struct timeval t1, t2;
	struct test_msg_t *msg = pcn_kmsg_alloc_msg(sizeof(*msg));

	msg->header.type= PCN_KMSG_TYPE_SELFIE_TEST;
	memset(msg->payload, 'b', payload_size);

	if (my_nid == 0)
		dst=1;

	do_gettimeofday(&t1);
	for (i=0; i<MAX_TESTING_SIZE/payload_size; i++)
		pcn_kmsg_send(dst, msg, payload_size + sizeof(msg->header));
	do_gettimeofday(&t2);

	if (t2.tv_usec-t1.tv_usec >= 0) {
		EXP_DATA("Send one-way: send payload size %llu, "
							"total size %d, %llu times, spent %ld.%06ld s\n",
				payload_size, MAX_TESTING_SIZE, MAX_TESTING_SIZE/payload_size, 
									t2.tv_sec-t1.tv_sec, t2.tv_usec-t1.tv_usec);
	} else {
		EXP_DATA("Send one-way: send payload size %llu, "
							"total size %d, %llu times, spent %ld.%06ld s\n",
				payload_size, MAX_TESTING_SIZE, MAX_TESTING_SIZE/payload_size,
					t2.tv_sec-t1.tv_sec-1, (1000000-(t1.tv_usec-t2.tv_usec)));
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
						unsigned long long iter, bool is_rdma_read)
{
	unsigned long long i, j;
	struct timeval t1, t2;

	do_gettimeofday(&t1);
	for(j=0; j<iter; j++) {
		for(i=0; i<MAX_NUM_NODES; i++) {
			remote_thread_rdma_rw_t* request_rdma;
			remote_thread_rdma_rw_t *res;
			struct wait_station *ws;

			if (my_nid==i)
				continue;

			request_rdma = pcn_kmsg_alloc_msg(sizeof(*request_rdma));
			if (!request_rdma)
				BUG();

			ws = get_wait_station(current);
			request_rdma->remote_ws = ws->id;

			if (is_rdma_read == true) {
				request_rdma->header.type =
										PCN_KMSG_TYPE_RDMA_READ_TEST_REQUEST;
				request_rdma->rdma_header.rmda_type_res =
										PCN_KMSG_TYPE_RDMA_READ_TEST_RESPONSE;
			} else {
				request_rdma->header.type =
										PCN_KMSG_TYPE_RDMA_WRITE_TEST_REQUEST;
				request_rdma->rdma_header.rmda_type_res =
										PCN_KMSG_TYPE_RDMA_WRITE_TEST_RESPONSE;
			}

			request_rdma->header.prio = PCN_KMSG_PRIO_NORMAL;

			/* msg essentials */
			/* ------------------------------------------------------------ */
			/* msg dependences */

			/* READ/WRITE specific */
			if (is_rdma_read == true)
				request_rdma->rdma_header.is_write = false;
			else
				request_rdma->rdma_header.is_write = true;

			request_rdma->rdma_header.your_buf_ptr = dummy_act_buf;

			pcn_kmsg_send_rdma(i, request_rdma,
							sizeof(*request_rdma), (unsigned int)payload_size);
			pcn_kmsg_free_msg(request_rdma);

			res = wait_at_station(ws);
			put_wait_station(ws);
			pcn_kmsg_free_msg(res);
			DEBUG_LOG_V("\n\n\n");
		}
	}
	do_gettimeofday(&t2);

	EXP_DATA("RDMA %s payload size %llu, %llu times, spent %ld.%06ld s\n",
							is_rdma_read?"READ":"WRITE",
							payload_size, iter,

							t2.tv_usec-t1.tv_usec >= 0?
							t2.tv_sec-t1.tv_sec:
							t2.tv_sec-t1.tv_sec-1,

							t2.tv_usec-t1.tv_usec >= 0?
							t2.tv_usec-t1.tv_usec:
							(1000000-(t1.tv_usec-t2.tv_usec)));

	return 0;
}

/*	mimic_read:
 *			=====>
 *					[copy]
 *			<-----
 *
 *	mimic_write:	(more often)
 *			----->
 *			<====
 *	 [copy]
 *
 */
void test_send_read_throughput(unsigned long long payload_size,
										 unsigned long long iter)
{
	int i, dst = 0;
	struct timeval t1, t2;

	if (my_nid == 0)
		dst=1;

	do_gettimeofday(&t1);
	for (i = 0; i < iter; i++) {
		struct wait_station *ws = get_wait_station(current);
		struct test_msg_request_t *req = __alloc_send_roundtrip_read_request();
		struct test_msg_signal_t *res;
		req->remote_ws = ws->id;
		req->size = payload_size;
		memcpy(&req->payload, dummy_send_buf, payload_size);

		DEBUG_CORRECTNESS("%s(): r local to remote size %llu = "
							"sizeof(*req)(%llu) - sizeof(req->payload)(%llu)"
							" + payload_size(%llu)\n",
				__func__, sizeof(*req) - sizeof(req->payload) + payload_size,
							sizeof(*req), sizeof(req->payload), payload_size);
		
		pcn_kmsg_send(dst, req,
							sizeof(*req) - sizeof(req->payload) + payload_size);

		pcn_kmsg_free_msg(req);
		res = wait_at_station(ws);
		put_wait_station(ws);

		pcn_kmsg_free_msg(res);
	}
	do_gettimeofday(&t2);

	_show_time(&t1, &t2, payload_size, iter, "Send roundtrip (READ)");
}


void test_send_write_throughput(unsigned long long payload_size,
											unsigned long long iter)
{
	int i, dst = 0;
	struct timeval t1, t2;

	if (my_nid == 0)
		dst=1;

	do_gettimeofday(&t1);
	for (i = 0; i < iter; i++) {
		struct wait_station *ws = get_wait_station(current);
		struct test_msg_signal_t *req = __alloc_send_roundtrip_write_request();
		struct test_msg_request_t *res;
		req->remote_ws = ws->id;
		req->size = payload_size;

		pcn_kmsg_send(dst, req, sizeof(*req));

		pcn_kmsg_free_msg(req);
		res = wait_at_station(ws);
		put_wait_station(ws);

		//printk("mimic WRITE\n");
		memcpy(dummy_send_buf, &res->payload, payload_size);

		pcn_kmsg_free_msg(res);
	}
	do_gettimeofday(&t2);
	_show_time(&t1, &t2, payload_size, iter, "Send roundtrip (WRITE)");
}

static int rdma_RW_inv_test(void* buf, unsigned long long payload_size,
														bool is_rdma_read)
{
	unsigned long long i;

	remote_thread_rdma_rw_t* request_rdma;
	request_rdma = pcn_kmsg_alloc_msg(sizeof(*request_rdma));
	if (!request_rdma)
		return -1;

	if (is_rdma_read == true) {
		request_rdma->header.type = PCN_KMSG_TYPE_RDMA_READ_TEST_REQUEST;
		request_rdma->rdma_header.rmda_type_res =
										PCN_KMSG_TYPE_RDMA_READ_TEST_RESPONSE;
	} else {
		request_rdma->header.type = PCN_KMSG_TYPE_RDMA_WRITE_TEST_REQUEST;
		request_rdma->rdma_header.rmda_type_res =
										PCN_KMSG_TYPE_RDMA_WRITE_TEST_RESPONSE;
	}
	request_rdma->header.prio = PCN_KMSG_PRIO_NORMAL;

	/* msg essentials */
	/* ------------------------------------------------------------ */
	/* msg dependences */

	/* READ/WRITE specific */
	if (is_rdma_read == true)
		request_rdma->rdma_header.is_write = false;
	else
		request_rdma->rdma_header.is_write = true;

	request_rdma->rdma_header.your_buf_ptr = buf;	// provided by user


	for(i=0; i<MAX_NUM_NODES; i++) {
		if (my_nid==i)
			continue;
		pcn_kmsg_send_rdma(i, request_rdma,
							sizeof(*request_rdma), (unsigned int)payload_size);
		DEBUG_LOG_V("\n\n\n");
	}
	msleep(1000); // wait for taking effect

	pcn_kmsg_free_msg(request_rdma);
	return 0;
}


static ssize_t write_proc(struct file * file, 
					const char __user * buffer, size_t count, loff_t *ppos)
{
	volatile int i;
	char *cmd;
	volatile int cnt = 0;
	struct task_struct *t;
	char *argv[MAX_ARGS_NUM];

	int args = 0;
	char *tok, *end;

	if (!try_module_get(THIS_MODULE))
		return -ENODEV;

	cmd = kmalloc(count, GFP_KERNEL);
	if (cmd == NULL) {
		printk(KERN_ERR "kmalloc failure\n");
		return -ENOMEM;
	}
	if (copy_from_user(cmd, buffer, count)) {
		return -EFAULT;
	}

	/* remove the \n */
	cmd[count - 1] = 0;

	/* start to parse */
	for(i=0; i<MAX_ARGS_NUM; i++) {
		argv[i]=NULL;
	}
	
	tok = cmd; end = cmd;
	while (tok != NULL) {
		argv[args] = strsep(&end, " ");
		KRPRINT_INIT("%s\n", tok);
		args++;
		tok = end;
	}
	
	for(i=0; i<MAX_ARGS_NUM; i++) {
		if (argv[i]!=NULL)
			KRPRINT_INIT("argv[%d]= %s\n", i, argv[i]);
	}

#ifdef CONFIG_POPCORN_MSG_STATISTIC
	printk(KERN_WARNING "You are tracking POPCORN_MSG_STATISTIC "
					"and geting inaccurate performance data now\n");
#endif
	
	KRPRINT_INIT("\n\n[ proc write |%s| cnt %ld ] [%d args] \n", 
												cmd, count, args);

	/* do the coresponding work */
	if (cmd[0]=='0') {
		printk("Reserved. Not implemented yet\n");
	}
	else if (cmd[0]=='1' && cmd[1]=='\0') {
		struct timeval t1;
		struct timeval t2;
		
		do_gettimeofday(&t1);
		while (++cnt<=TEST1_MSG_COUNT)
			test1();
		do_gettimeofday(&t2);
		if ( t2.tv_usec-t1.tv_usec >= 0) {
			EXP_DATA("Send throughput result: send msg size %d, "
					"total size %d, %d times, spent %ld.%06ld s\n",
					TEST1_PAYLOAD_SIZE, MAX_TESTING_SIZE, TEST1_MSG_COUNT,
								t2.tv_sec-t1.tv_sec, t2.tv_usec-t1.tv_usec);
		} else {
			EXP_DATA("Send throughput result: send msg size %d, "
					"total size %d, %d times, spent %ld.%06ld s\n",
					TEST1_PAYLOAD_SIZE, MAX_TESTING_SIZE, TEST1_MSG_COUNT,
					t2.tv_sec-t1.tv_sec-1, (1000000-(t1.tv_usec-t2.tv_usec)));
		}
		EXP_LOG("test%c done\n\n\n\n", cmd[0]);
	}
	else if (cmd[0]=='2' && cmd[1]=='\0') {
		setup_read_buf();
		while (++cnt<=5000)
			test2();
		EXP_LOG("test%c done\n\n\n\n", cmd[0]);
	}
	else if (cmd[0]=='3' && cmd[1]=='\0') {
		setup_write_buf();
		while (++cnt<=5000)
			test3();
		EXP_LOG("test%c done\n\n\n\n", cmd[0]);
	}
	else if (cmd[0]=='4' && cmd[1]=='\0') { // conccurent multithreading test1()
		for(i=0; i<10; i++) {
			t = kthread_run(kthread_test1, NULL, "kthread_test1()");
			BUG_ON(IS_ERR(t));
		}
		EXP_LOG("test%c done\n\n\n\n", cmd[0]);
	}
	else if (cmd[0]=='5' && cmd[1]=='\0') { // conccurent multithreading test2()
		setup_read_buf();
		for(i=0; i<10; i++) {
			t = kthread_run(kthread_test2, NULL, "kthread_test2()");
			BUG_ON(IS_ERR(t));
		}
		EXP_LOG("test%c done\n\n\n\n", cmd[0]);
	}
	else if (cmd[0]=='6' && cmd[1]=='\0') { // conccurent multithreading test3()
		setup_write_buf();
		for(i=0; i<10; i++) {
			t = kthread_run(kthread_test3, NULL, "kthread_test3()");
			BUG_ON(IS_ERR(t));
		}
		EXP_LOG("test%c done\n\n\n\n", cmd[0]);
	}
	else if (cmd[0]=='9' && cmd[1]=='\0') {
		// conccurent multithreading test1&2&3() at the same time
		setup_read_buf();
		for(i=0; i<10; i++) {
			t = kthread_run(kthread_test1, NULL, "kthread_test1()");
			BUG_ON(IS_ERR(t));
			t = kthread_run(kthread_test2, NULL, "kthread_test2()");
			BUG_ON(IS_ERR(t));
			t = kthread_run(kthread_test3, NULL, "kthread_test3()");
			BUG_ON(IS_ERR(t));
		}
		EXP_LOG("test%c done\n\n\n\n", cmd[0]);
	}
	else if (!memcmp(argv[0], "10", 2)) { // EXP DATA - SEND
		struct timeval t1, t2;
		if (args!=2) {
			printk(KERN_ERR "sudo echo 10 <SIZE> > /proc/kmsg_test\n");
			return count;
		}
		KRPRINT_INIT("arg %llu\n", simple_strtoull(argv[1], NULL, 0));
		for(i=0; i<ITER; i++) {
			do_gettimeofday(&t1);
			test_send_throughput(simple_strtoull(argv[1], NULL, 0));
			do_gettimeofday(&t2);
		}
		EXP_LOG("test %c%c test_send_throughput() done\n\n\n", 
														cmd[0], cmd[1]);
	}
	else if (!memcmp(argv[0], "11", 2) || !memcmp(argv[0], "12", 2)) { // EXP
		if (args<2) {
			printk(KERN_ERR "sudo echo 11/12 <SIZE> <ITER> > /proc/kmsg_test\n");
			return count;
		}

		if (simple_strtoull(argv[2], NULL, 0) == 0)
			printk(KERN_WARNING "iter = 0\n");

		KRPRINT_INIT("arg %llu\n", simple_strtoull(argv[1], NULL, 0));
		for(i=0; i<ITER; i++) {
			if (!memcmp(argv[0], "11", 2))
				test_send_read_throughput(simple_strtoull(argv[1], NULL, 0),
											simple_strtoull(argv[2], NULL, 0));
			else
				test_send_write_throughput(simple_strtoull(argv[1], NULL, 0),
											 simple_strtoull(argv[2], NULL, 0));
		}
		EXP_LOG("test %s test_send_roundtrip_throughput() done\n\n\n", argv[0]);
	}
	else if (!memcmp(argv[0], "13", 2) || !memcmp(argv[0], "14", 2)) {

		rdma_RW_test(simple_strtoull(argv[1], NULL, 0),
						simple_strtoull(argv[2], NULL, 0),
						!memcmp(argv[0], "13", 2)?true:false);
		EXP_LOG("test RDMA %s rdma_RW_test() done\n\n\n",
									!memcmp(argv[0], "13", 2)?"READ":"WRITE");
	}
	else if (!memcmp(argv[0], "15", 2)) {
		/* Because of this LENGTH is not divided by 10,
		 * the last few chars are not changed!			*/
		int z, jack = MAX_MSG_LENGTH/10;

		printk("----------------------------------\n");
		printk("----- READ test sanity start -----\n");
		printk("----------------------------------\n\n");

		/* init act_buf */
		for(z=0; z<10; z++) {
			memset(dummy_act_buf+(z*jack), z+'a', jack);
			printk("z %d act_buf \"%.10s\"\n", z, dummy_act_buf+(z*jack));
		}

		/* READ test */
		for(z=0; z<10; z++) {
			rdma_RW_inv_test(dummy_act_buf+(z*jack), jack, 1); //is read
			show_RW_dummy_buf();
			msleep(3000);
		}

		printk("---------------------------------\n");
		printk("----- READ test sanity done -----\n");
		printk("---------------------------------\n\n");
		msleep(5000);
		init_RW_dummy_buf();
		show_RW_dummy_buf();
		msleep(5000);

		printk("----------------------------------\n");
		printk("----- WRITE test sanity start ----\n");
		printk("----------------------------------\n\n");

		for(z=0; z<10; z++) {
			rdma_RW_inv_test(dummy_act_buf+(z*jack), jack, 0); //is read
			show_RW_dummy_buf();
			msleep(3000);
		}
		printk("----------------------------------\n");
		printk("----- WRITE test sanity done -----\n");
		printk("----------------------------------\n\n");
	}
	else if (!memcmp(argv[0], "16", 2)) {
		init_RW_dummy_buf();
	}
	else if (!memcmp(argv[0], "20", 2)) {
		show_RW_dummy_buf();
	}
	else {
		printk("Not support yet. Try \"1,2,3,4,5,6,9,10\"\n");
	}

	kfree(cmd);
	module_put(THIS_MODULE);
	KRPRINT_INIT("proc write done!!\n");
	return count;	// if not reach count, will reenter again
}

static ssize_t read_func(struct file *filp, char *usr_buf,
									size_t count, loff_t *offset)
{
#ifdef CONFIG_POPCORN_MSG_STATISTIC
	int i;
#endif
	char *buf;
	int len = 0;

	buf = kzalloc(PROC_BUF_SIZE, GFP_KERNEL);
	if (!buf)
		BUG();

	if (*offset > 0)
		return 0;
	
#ifdef CONFIG_POPCORN_MSG_STATISTIC
	for (i=1; i<MAX_STATISTIC_SLOTS; i++) {
		if (atomic_read(&send_pattern[i]) == 0)
			continue;
		len += snprintf(buf+strlen(buf), PROC_BUF_SIZE,
						"SEND: size %d cnt %lu\n", i,
						(unsigned long)atomic_read(&send_pattern[i]));
	}
	for (i=1; i<MAX_STATISTIC_SLOTS; i++) {
		if (atomic_read(&recv_pattern[i]) == 0)
			continue;
		len += snprintf(buf+strlen(buf), PROC_BUF_SIZE,
						"RECV: size %d cnt %lu\n", i,
						(unsigned long)atomic_read(&recv_pattern[i]));
	}

	if ( len > PROC_BUF_SIZE ) {
		len = PROC_BUF_SIZE;
		printk(KERN_WARNING "logs dropped\n");
	}

	if (copy_to_user(usr_buf, buf, len)) {
		printk(KERN_ERR "cpy_2_usr failed\n");
		kfree(buf);
		return -EFAULT;
	}

#else
	printk("Please turn on CONFIG_POPCORN_MSG_STATISTIC "
								"and recompile the kernel agaian!!\n");
	len += snprintf(buf+strlen(buf), PROC_BUF_SIZE,
					"Please turn on CONFIG_POPCORN_MSG_STATISTIC "
								"and recompile the kernel agaian!!\n");
#endif

	kfree(buf);
	*offset = len;
	return len;
}

static int kmsg_test_read_proc(struct seq_file *seq, void *v)
{
	printk("Not suppor open\n");
	return 0;
}

static int kmsg_test_read_open(struct inode *inode, struct file *file)
{
	return single_open(file, kmsg_test_read_proc, inode->i_private);
}

static struct file_operations kmsg_test_ops = {
	.owner = THIS_MODULE,
	.open = kmsg_test_read_open,
	.read = read_func,
	.llseek  = seq_lseek,
	.release = single_release,
	.write = write_proc,
};


/**
 *	Too large to static allocate - if do statically, array drain happens
 **/
static void __reply_send_r_roundtrip(struct test_msg_request_t *req, int ret)
{
	struct test_msg_signal_t *res = pcn_kmsg_alloc_msg(sizeof(*res));
	res->header.type = PCN_KMSG_TYPE_SEND_ROUND_RESPONSE;
	res->header.prio = PCN_KMSG_PRIO_NORMAL;
	res->remote_ws = req->remote_ws;
	//res->size = req->size;
	pcn_kmsg_send(req->header.from_nid, res, sizeof(*res));
	pcn_kmsg_free_msg(res);
}

static void process_send_roundtrip_request(struct work_struct *_work)
{
	struct pcn_kmsg_work *work = (struct pcn_kmsg_work *)_work;
	struct test_msg_request_t *req = work->msg;

	memcpy(dummy_send_buf, &req->payload, req->size);

	__reply_send_r_roundtrip(req, -EINVAL);

	pcn_kmsg_free_msg(req);
	kfree(work);
}

static void process_send_roundtrip_response(struct work_struct *_work)
{
	struct pcn_kmsg_work *work = (struct pcn_kmsg_work *)_work;
	struct test_msg_signal_t *res = work->msg;
	struct wait_station *ws = wait_station(res->remote_ws);

	ws->private = res;
	smp_mb();
	complete(&ws->pendings);

	kfree(work);
}

/**
 *	Too large to static allocate - if do statically, array drain happens
 **/
static void __reply_send_w_roundtrip(struct test_msg_signal_t *req, int ret)
{
	struct test_msg_request_t *res = pcn_kmsg_alloc_msg(sizeof(*res));
	res->header.type = PCN_KMSG_TYPE_SEND_ROUND_WRITE_RESPONSE;
	res->header.prio = PCN_KMSG_PRIO_NORMAL;
	res->remote_ws = req->remote_ws;
	res->size = req->size;

	DEBUG_CORRECTNESS("%s(): w remote back size %d = "
			"sizeof(*res)%d - sizeof(res->payload)%d + req->size %d\n",
			__func__, sizeof(*res) - sizeof(res->payload) + req->size,
			sizeof(*res), sizeof(res->payload), req->size);

	/* mimic WRITE */
	memcpy(&res->payload, dummy_send_buf, req->size);
	pcn_kmsg_send(req->header.from_nid, res,
					sizeof(*res) - sizeof(res->payload) + req->size);
	pcn_kmsg_free_msg(res);
}

static void process_send_roundtrip_w_request(struct work_struct *_work)
{
	struct pcn_kmsg_work *work = (struct pcn_kmsg_work *)_work;
	struct test_msg_signal_t *req = work->msg;

	__reply_send_w_roundtrip(req, -EINVAL);

	pcn_kmsg_free_msg(req);
	kfree(work);
}

static void process_send_roundtrip_w_response(struct work_struct *_work)
{
	struct pcn_kmsg_work *work = (struct pcn_kmsg_work *)_work;
	struct test_msg_request_t *res = work->msg;
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
	void* paddr = dummy_pass_buf;

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
	void* paddr = dummy_pass_buf;

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


DEFINE_KMSG_WQ_HANDLER(send_roundtrip_request);
DEFINE_KMSG_NONBLOCK_WQ_HANDLER(send_roundtrip_response);

DEFINE_KMSG_WQ_HANDLER(send_roundtrip_w_request);
DEFINE_KMSG_NONBLOCK_WQ_HANDLER(send_roundtrip_w_response);

DEFINE_KMSG_WQ_HANDLER(handle_test_read_request);
DEFINE_KMSG_NONBLOCK_WQ_HANDLER(handle_test_read_response);

DEFINE_KMSG_WQ_HANDLER(handle_test_write_request);
DEFINE_KMSG_NONBLOCK_WQ_HANDLER(handle_test_write_response);

/* example - main usage */
static int __init msg_test_init(void)
{
	static struct proc_dir_entry *kmsg_test_proc;

	/* register a proc */
	printk("\n\n--- Popcorn messaging self testing proc init ---\n");
	kmsg_test_proc = proc_create("kmsg_test", 0666, NULL, &kmsg_test_ops);
	if (kmsg_test_proc == NULL) {
		printk(KERN_ERR "cannot create /proc/kmsg_test\n");
		return -ENOMEM;
	}

	dummy_send_buf = kzalloc(MAX_MSG_LENGTH, GFP_KERNEL);
	if (!dummy_send_buf) BUG();

	memset(dummy_send_buf, 'S', MAX_MSG_LENGTH/2);
	memset(dummy_send_buf+(MAX_MSG_LENGTH/2), 'T', MAX_MSG_LENGTH/2);

	/* register callback. also define in <linux/pcn_kmsg.h>  */
	pcn_kmsg_register_callback((enum pcn_kmsg_type)PCN_KMSG_TYPE_FIRST_TEST,
					(pcn_kmsg_cbftn)handle_remote_thread_first_test_request);

	/* for experimental data - send throughput */
	pcn_kmsg_register_callback(PCN_KMSG_TYPE_SELFIE_TEST, handle_self_test);
	pcn_kmsg_register_callback(PCN_KMSG_TYPE_SHOW_REMOTE_TEST_BUF,
								(pcn_kmsg_cbftn)handle_show_RW_dummy_buf);

	/* for experimental data - Round-trip throughput */
	REGISTER_KMSG_WQ_HANDLER(PCN_KMSG_TYPE_SEND_ROUND_REQUEST,
													send_roundtrip_request);
	REGISTER_KMSG_WQ_HANDLER(PCN_KMSG_TYPE_SEND_ROUND_RESPONSE,
													send_roundtrip_response);

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

	printk("--- Popcorn messaging layer self-testing proc init done ---\n");
	printk("--- Usage: sudo echo [NUM] [DEPENDS] > /proc/kmsg_test ---\n");
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
	printk("---      ex: echo 11 [SIZE] [ITER] > /proc/kmsg_test ---\n");
	printk("---  12: single thread send throughput (round-trip - "
												"simulate RDMA WRITE) ---\n");
	printk("---      ex: echo 12 [SIZE] [ITER] > /proc/kmsg_test ---\n");
	printk("---  13: RDMA READ a page throughput  ---\n");
	printk("---      ex: echo 13 [SIZE] [ITER] > /proc/kmsg_test ---\n");
	printk("---  14: RDMA WRITE a page throughput  ---\n");
	printk("---      ex: echo 14 [SIZE] [ITER] > /proc/kmsg_test ---\n");
	printk("---  15: RDMA READ invalidation test (10 buf of 8192 size)  ---\n");
	printk("---      ex: echo 15 > /proc/kmsg_test ---\n");
	printk("=============== msg_layer usage pattern  ===============\n");
	printk("---  cat: showing msg_layer usage pattern ---\n");
	printk("---      ex: cat /proc/kmsg_test ---\n");
	printk("\n\n\n\n\n\n\n\n");
	smp_mb();
	return 0;
}

static void __exit msg_test_exit(void) 
{
	printk("\n\n--- Popcorn messaging self testing unloaded! ---\n\n");
	if (g_test_buf)
		kfree(g_test_buf);
	if (g_test_write_buf)
		kfree(g_test_write_buf);
	remove_proc_entry("kmsg_test", NULL);
}

module_init(msg_test_init);
module_exit(msg_test_exit);
MODULE_LICENSE("GPL");
