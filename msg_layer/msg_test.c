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
#include <popcorn/pcn_kmsg.h>

//#include <popcorn/types.h>
#include "../../kernel/popcorn/types.h"
#include <popcorn/bundle.h>

#include "common.h"
#include "../../kernel/popcorn/wait_station.h"

/* testing args */
#define ITER 1					// iter for test10 (send throughput)
#define MAX_ARGS_NUM 10	
#define MAX_MSG_LENGTH 16384	// max msg payload size supported by msg_test.c 
#define TEST1_MSG_COUNT 100000	// specifically for test1 iterations
char dump_buf[MAX_MSG_LENGTH];

/* proc output args */
//#define MAX_STATISTIC_SLOTS 100	// support n diffrent sizes of msg
#define PROC_BUF_SIZE 1024*1024 // proc output size

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
//#define DEBUG_LOG_V(...) trace_printk(__VA_ARGS__)
#else
#define MSG_RDMA_PRK(...)
#define KRPRINT_INIT(...)
#define MSG_SYNC_PRK(...)
#define DEBUG_LOG_V(...)
#endif 

#define MAX_TESTING_SIZE 125829120 // = 120*1024*1024
#define TEST1_PAYLOAD_SIZE 1024

/* for testing rdma read */
int g_remote_read_len = 4*1024; // testing size for RDMA, mimicing user buf size
int g_rdma_write_len;			// size wanna read/write
char *g_test_buf = NULL;		// mimicing user buf

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
	bool is_mimic_read;
	unsigned char payload[MAX_MSG_LENGTH];
};

struct test_msg_response_t {
	struct pcn_kmsg_hdr header;
	int remote_ws;
};



/* example - handler */
static void handle_remote_thread_first_test_request(
									struct pcn_kmsg_long_message* inc_lmsg)
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

	//DEBUG_LOG_V("%s(): %s\n", __func__, request->payload);

	pcn_kmsg_free_msg(request);
	return 0;
}




static struct test_msg_request_t *__alloc_send_roundtrip_request(void)
{
    struct test_msg_request_t *req = pcn_kmsg_alloc_msg(sizeof(struct test_msg_request_t));
    if(req == NULL)
		return NULL;

	req->header.type = PCN_KMSG_TYPE_SEND_ROUND_REQUEST;
	req->header.prio = PCN_KMSG_PRIO_NORMAL;
    return req;
}

void roundtrip_show_time(	struct timeval *t1, struct timeval *t2,
							unsigned long long payload_size,
							unsigned long long iter)
{
	if( t2->tv_usec-t1->tv_usec >= 0) {
		EXP_DATA("Send roundtrip: send payload size %llu, "
							"%llu times, spent %ld.%06ld s\n",
							payload_size, iter, t2->tv_sec-t1->tv_sec,
							t2->tv_usec-t1->tv_usec);
	} else {
		EXP_DATA("Send roundtrip: send payload size %llu, "
							"%llu times, spent %ld.%06ld s\n",
							payload_size, iter, t2->tv_sec-t1->tv_sec-1,
							(1000000-(t1->tv_usec-t2->tv_usec)));
	}
}

void test_send_roundtrip_throughput(unsigned long long payload_size,
									unsigned long long iter,
									bool is_mimic_read)
{
	int i, dst = 0;
	struct timeval t1, t2;
	struct test_msg_request_t *req = __alloc_send_roundtrip_request();
	struct test_msg_request_t *res;

	req->is_mimic_read = is_mimic_read;
	memset(req->payload, 'b', payload_size);

	if (my_nid == 0)
		dst=1;

	do_gettimeofday(&t1);
	for (i = 0; i < iter; i++) { //
		struct wait_station *ws = get_wait_station(current);
		req->remote_ws = ws->id;
		pcn_kmsg_send(dst, req, payload_size + sizeof(struct pcn_kmsg_hdr));
		res = wait_at_station(ws);
		put_wait_station(ws);

		if(!is_mimic_read) {
			//printk("mimic WRITE\n");
			// round-trip simulaties WRITE
			memcpy(&dump_buf, &res->payload,
								res->header.size - sizeof(struct pcn_kmsg_hdr));
		}
		pcn_kmsg_free_msg(res);
	}
	do_gettimeofday(&t2);
	roundtrip_show_time(&t1, &t2, payload_size, iter);
}

static int test1(void)
{
	/* ----- 1st testing -----
	 * 	[we are here]
	 *	[compose]
	 *  send       ---->   irq (recv)
	 *  [done]
	 */

	int i;
	static int cnt = 0;
	
	remote_thread_first_test_request_t* request; // youTODO: make your own struct 
	request = pcn_kmsg_alloc_msg(sizeof(*request));
	if(request==NULL)
		return -1;

	request->header.type = PCN_KMSG_TYPE_FIRST_TEST;
	request->header.prio = PCN_KMSG_PRIO_NORMAL;

	/* msg essentials */
	/* ------------------------------------------------------------ */
	/* msg dependences */
	request->example1 = my_nid;        // doesn't solve the problem
	request->example2 = ++cnt;
	memset(request->msg,'J', sizeof(request->msg));
	DEBUG_LOG_V("\n%s(): example2(t) %d strlen(request->msg) %d "
						"to all others\n", __func__, request->example2,
												(int)strlen(request->msg));

	for(i=0; i<MAX_NUM_NODES; i++) {
		if(my_nid==i)
			continue;
		pcn_kmsg_send(i, (struct pcn_kmsg_long_message*) request,
														sizeof(*request));
	}

	pcn_kmsg_free_msg(request);
	return 0;
}

/* ===== 2nd testing: r_read =====
 * 	[we are here]
 *	[compose]
 *  send       ---->   irq (recv)
 *                      perform READ
 * irq (recv)  <-----   send
 * 
 */
static int test2(void)
{
	volatile int i;

	remote_thread_rdma_rw_request_t* request_rdma_read;
	//request_rdma_read = pcn_kmsg_alloc_msg(sizeof(*request_rdma_read));
	request_rdma_read = kmalloc(sizeof(*request_rdma_read), GFP_KERNEL);
	if(request_rdma_read==NULL)
		return -1;

	request_rdma_read->header.type = PCN_KMSG_TYPE_RDMA_READ_REQUEST;
	request_rdma_read->header.prio = PCN_KMSG_PRIO_NORMAL;

	/* msg essentials */
	/* ------------------------------------------------------------ */
	/* msg dependences */

	/* READ/WRITE specific: *buf, size */
	request_rdma_read->is_write = false;
	

	request_rdma_read->rw_size = g_remote_read_len;	// size you wanna passive remote to read //testing
	/* g_test_buf is done by setup_read_buf() */
	request_rdma_read->your_buf_ptr = g_test_buf;		// must be kmalloc()ed	
	/*
	 * your buf will be copied to rdma buf for a passive remote read
	 * user should protect
	 * local buffer size for passive remote to read
	 */
	for(i=0; i<MAX_NUM_NODES; i++) {
		if(my_nid==i)
			continue;
		
		pcn_kmsg_send_rdma(i, (struct pcn_kmsg_long_message*)request_rdma_read, g_remote_read_len);
		DEBUG_LOG_V("\n\n\n"); 
	}
	//pcn_kmsg_free_msg(request_rdma_read);
	kfree(request_rdma_read);
	return 0;
}

/* ===== 3rd testing: r_write =====
 * 	[we are here]
 *	[compose]
 *  send       ---->   irq (recv)
 *                      perform WRITE
 * irq (recv)  <-----   send
 * 
 */
static int test3(void)
{
	int i;

	remote_thread_rdma_rw_request_t* request_rdma_write;
	//request_rdma_write = pcn_kmsg_alloc_msg(sizeof(*request_rdma_write));
	request_rdma_write = kmalloc(sizeof(*request_rdma_write), GFP_KERNEL);

	if(request_rdma_write==NULL)
		return -1;

	request_rdma_write->header.type = PCN_KMSG_TYPE_RDMA_WRITE_REQUEST;
	request_rdma_write->header.prio = PCN_KMSG_PRIO_NORMAL;

	/* msg essentials */
	/* ------------------------------------------------------------ */
	/* msg dependences */

	/* READ/WRITE specific */
	request_rdma_write->is_write = true;
	request_rdma_write->rw_size = g_remote_read_len; //testing
									// size you wanna passive remote to WRITE

	for(i=0; i<MAX_NUM_NODES; i++) {
		if(my_nid==i)
			continue;
		
		g_rdma_write_len = 4*1024; // size wanna remote to perform read/write
		pcn_kmsg_send_rdma(i, (struct pcn_kmsg_long_message*)request_rdma_write, g_rdma_write_len);
		
		DEBUG_LOG_V("\n\n\n"); 
	}
	//pcn_kmsg_free_msg(request_rdma_write);
	kfree(request_rdma_write);
	return 0;
}

//static void kthread_test1(void* arg0)
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
	struct test_msg_t *msg;

	msg = pcn_kmsg_alloc_msg(sizeof(struct test_msg_t));
	msg->header.type= PCN_KMSG_TYPE_SELFIE_TEST;
	memset(msg->payload, 'b', payload_size);

	if (my_nid == 0)
		dst=1;

	do_gettimeofday(&t1);
	for (i = 0; i < MAX_TESTING_SIZE/payload_size; i++)
		pcn_kmsg_send(dst, msg, payload_size + sizeof(msg->header));
	do_gettimeofday(&t2);

	if( t2.tv_usec-t1.tv_usec >= 0) {
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

#ifdef RDMAWR
/* ===== Jack testing: page READ/WRITE =====
 * 	[we are here]
 *	[compose]
 *  *remap addr*
 *  send       ---->   irq (recv)
 *                      perform WRITE
 * irq (recv)  <-----   send
 * 
 */
static int page_RW_test(void)
{
	int i;

	remote_thread_rdma_rw_request_t* request_rdma_write;
	//request_rdma_write = pcn_kmsg_alloc_msg(sizeof(*request_rdma_write));
	request_rdma_write = kmalloc(sizeof(*request_rdma_write), GFP_KERNEL);

	if(request_rdma_write==NULL)
		return -1;

	request_rdma_write->header.type = PCN_KMSG_TYPE_RDMA_WRITE_REQUEST;
	request_rdma_write->header.prio = PCN_KMSG_PRIO_NORMAL;

	/* msg essentials */
	/* ------------------------------------------------------------ */
	/* msg dependences */

	/* READ/WRITE specific */
	request_rdma_write->is_write = true;
	request_rdma_write->rw_size = g_remote_read_len; //testing
									// size you wanna passive remote to WRITE

	for(i=0; i<MAX_NUM_NODES; i++) {
		if(my_nid==i)
			continue;
		
		g_rdma_write_len = 4*1024; // size wanna remote to perform read/write
		pcn_kmsg_send_rdma(i, (struct pcn_kmsg_long_message*)request_rdma_write, g_rdma_write_len);
		
		DEBUG_LOG_V("\n\n\n"); 
	}
	//pcn_kmsg_free_msg(request_rdma_write);
	kfree(request_rdma_write);
	return 0;
}
#endif

/* testing utility */
/*
 * in the reality, this function should be called between send()s
 */
int setup_read_buf(void)
{
	volatile int i;
	// read specific (data you wanna let remote side read)
	g_test_buf = kmalloc(g_remote_read_len, GFP_KERNEL);
	if (!g_test_buf) {
		printk(KERN_ERR "kmalloc failure\n");
		return -ENOMEM;
	}
	memset(g_test_buf, 'R', g_remote_read_len); 
							// user data buffer ( will be copied to rdma buf)
																			
	for(i=0; i<MAX_NUM_NODES; i++) {
		if(i==my_nid)
			continue; 	// canot write to its rw_active_buf since addr space
	}	
	
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
		if(argv[i]!=NULL)
			KRPRINT_INIT("argv[%d]= %s\n", i, argv[i]);
	}
	
	KRPRINT_INIT("\n\n[ proc write |%s| cnt %ld ] [%d args] \n", 
												cmd, count, args);

	/* do the coresponding function */
	if(cmd[0]=='0') {
		// Can reset log
		//for (i=0; i<MAX_NUM_NODES; i++)
			//atomic_set(&cb[i]->stats.send_msgs, 0);
		printk("Reserved. Not implemented yet\n");
		EXP_LOG("cmd%c clean all done\n\n\n\n", cmd[0]);
	}
	else if(cmd[0]=='1' && cmd[1]=='\0') {
		struct timeval t1;
		struct timeval t2;
		
		do_gettimeofday(&t1);
		while(++cnt<=TEST1_MSG_COUNT)
			test1();
		do_gettimeofday(&t2);
		if( t2.tv_usec-t1.tv_usec >= 0) {
			EXP_DATA("Send throughput result: send msg size %d, "
					"total size %d, %d times, spent %ld.%06ld s\n",
					TEST1_PAYLOAD_SIZE, MAX_TESTING_SIZE, TEST1_MSG_COUNT,
								t2.tv_sec-t1.tv_sec, t2.tv_usec-t1.tv_usec);
		} else {
			EXP_DATA("Send throughput result: size %d, "
					"total size %d, %d times, spent %ld.%06ld s\n",
					TEST1_PAYLOAD_SIZE, MAX_TESTING_SIZE, TEST1_MSG_COUNT,
					t2.tv_sec-t1.tv_sec-1, (1000000-(t1.tv_usec-t2.tv_usec)));
		}
		EXP_LOG("test%c done\n\n\n\n", cmd[0]);
	}
	else if(cmd[0]=='2' && cmd[1]=='\0') {
		setup_read_buf();
		while(++cnt<=5000)
			test2();
		EXP_LOG("test%c done\n\n\n\n", cmd[0]);
	}
	else if(cmd[0]=='3' && cmd[1]=='\0') {
		while(++cnt<=5000)
			test3();
		EXP_LOG("test%c done\n\n\n\n", cmd[0]);
	}
	else if(cmd[0]=='4' && cmd[1]=='\0') { // conccurent multithreading test1()
		for(i=0; i<10; i++) {
			t = kthread_run(kthread_test1, NULL, "kthread_test1()");
			BUG_ON(IS_ERR(t));
		}
		EXP_LOG("test%c done\n\n\n\n", cmd[0]);
	}
	else if(cmd[0]=='5' && cmd[1]=='\0') { // conccurent multithreading test2()
		setup_read_buf();
		for(i=0; i<10; i++) {
			t = kthread_run(kthread_test2, NULL, "kthread_test2()");
			BUG_ON(IS_ERR(t));
		}
		EXP_LOG("test%c done\n\n\n\n", cmd[0]);
	}
	else if(cmd[0]=='6' && cmd[1]=='\0') { // conccurent multithreading test3()
		for(i=0; i<10; i++) {
			t = kthread_run(kthread_test3, NULL, "kthread_test3()");
			BUG_ON(IS_ERR(t));
		}
		EXP_LOG("test%c done\n\n\n\n", cmd[0]);
	}
	else if(cmd[0]=='9' && cmd[1]=='\0') {
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
	else if(!memcmp(argv[0], "10", 2)) { // EXP DATA - SEND
		struct timeval t1, t2;
		if(args!=2) {
			printk(KERN_ERR "sudo echo 10 <SIZE> > /proc/kmsg_test\n");
			return count;
		}
#ifdef CONFIG_POPCORN_MSG_STATISTIC
		printk(KERN_WARNING "You are tracking POPCORN_MSG_STATISTIC "
				"and geting inaccurate performance data now\n");
#endif
		KRPRINT_INIT("arg %llu\n", simple_strtoull(argv[1], NULL, 0));
		for(i=0; i<ITER; i++) {
			do_gettimeofday(&t1);
			test_send_throughput(simple_strtoull(argv[1], NULL, 0));
			do_gettimeofday(&t2);
		}
		EXP_LOG("test %c%c test_send_throughput() done\n\n\n", 
														cmd[0], cmd[1]);
	}
	else if(!memcmp(argv[0], "11", 2) || !memcmp(argv[0], "12", 2)) { // EXP
		if(args<2) {
			printk(KERN_ERR "sudo echo 11 <SIZE> > /proc/kmsg_test\n");
			return count;
		}
		if(simple_strtoull(argv[2], NULL, 0) == 0)
			printk(KERN_WARNING "iter = 0\n");

#ifdef CONFIG_POPCORN_MSG_STATISTIC
		printk(KERN_WARNING "You are tracking POPCORN_MSG_STATISTIC "
				"and geting inaccurate performance data now\n");
#endif
		KRPRINT_INIT("arg %llu\n", simple_strtoull(argv[1], NULL, 0));
		for(i=0; i<ITER; i++) {
			test_send_roundtrip_throughput(simple_strtoull(argv[1], NULL, 0),
										simple_strtoull(argv[2], NULL, 0),
										memcmp(argv[0], "11", 2)==0?true:false);
		}
		EXP_LOG("test %s test_send_throughput() done\n\n\n", argv[0]);
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
	if(!buf)
		BUG();

	if (*offset > 0)
		return 0;
	
#ifdef CONFIG_POPCORN_MSG_STATISTIC
	for (i=1; i<MAX_STATISTIC_SLOTS; i++) {
		if(atomic_read(&send_pattern[i]) == 0)
			continue;
		len += snprintf(buf+strlen(buf), PROC_BUF_SIZE,
                        "SEND: size %d cnt %lu\n", i,
                        (unsigned long)atomic_read(&send_pattern[i]));
	}
	for (i=1; i<MAX_STATISTIC_SLOTS; i++) {
		if(atomic_read(&recv_pattern[i]) == 0)
			continue;
		len += snprintf(buf+strlen(buf), PROC_BUF_SIZE,
                        "RECV: size %d cnt %lu\n", i,
                        (unsigned long)atomic_read(&recv_pattern[i]));
	}

	if( len > PROC_BUF_SIZE ) {
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
	printk("Not suppor open/read\n");
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
 * VMA worker
 *
 * We do this stupid thing because functions related to meomry mapping operate
 * on "current". Thus, we need mmap/munmap/madvise in our process
 */
static void __reply_send_roundtrip(struct test_msg_request_t *req, int ret)
{
    struct test_msg_response_t res = {
        .header = {
			.type = PCN_KMSG_TYPE_SEND_ROUND_RESPONSE,
            .prio = PCN_KMSG_PRIO_NORMAL,
        },
        .remote_ws = req->remote_ws,
    };
    pcn_kmsg_send(req->header.from_nid, &res, sizeof(res));
    //pcn_kmsg_send(req->remote_nid, &res, sizeof(res));
}

static void process_send_roundtrip_request(struct work_struct *_work)
{
	struct pcn_kmsg_work *work = (struct pcn_kmsg_work *)_work;
	struct test_msg_request_t *req = work->msg;

	if(req->is_mimic_read) {
		//printk("mimic READ\n");
		// round-trip simulaties READ
		memcpy(&dump_buf, &req->payload,
					req->header.size - sizeof(struct pcn_kmsg_hdr));
	}

	__reply_send_roundtrip(req, -EINVAL);

	pcn_kmsg_free_msg(req);
    kfree(work);
}

static void process_send_roundtrip_response(struct work_struct *_work)
{
    struct pcn_kmsg_work *work = (struct pcn_kmsg_work *)_work;
    struct test_msg_response_t *res = work->msg;
    struct wait_station *ws = wait_station(res->remote_ws);

    ws->private = res;
    smp_mb();
    complete(&ws->pendings);

    kfree(work);
}

DEFINE_KMSG_WQ_HANDLER(send_roundtrip_request);
DEFINE_KMSG_NONBLOCK_WQ_HANDLER(send_roundtrip_response);

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

	/* register callback. also define in <linux/pcn_kmsg.h>  */
	pcn_kmsg_register_callback((enum pcn_kmsg_type)PCN_KMSG_TYPE_FIRST_TEST,// ping - 
					(pcn_kmsg_cbftn)handle_remote_thread_first_test_request);
	//pcn_kmsg_register_callback(PCN_KMSG_TYPE_FIRST_TEST_RESPONSE,			// pong - usually a pair but just simply test here
	//								handle_remote_thread_first_test_response);

	// for experiments - send throughput
	pcn_kmsg_register_callback(PCN_KMSG_TYPE_SELFIE_TEST, handle_self_test);

	// for data
    REGISTER_KMSG_WQ_HANDLER(PCN_KMSG_TYPE_SEND_ROUND_REQUEST,
													send_roundtrip_request);
    REGISTER_KMSG_WQ_HANDLER(PCN_KMSG_TYPE_SEND_ROUND_RESPONSE,
													send_roundtrip_response);



	smp_mb(); // Just in case
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
	printk("---      ex: echo 11 [SIZE] > /proc/kmsg_test ---\n");
	printk("---  12: single thread send throughput (round-trip - "
												"simulate RDMA WRITE) ---\n");
	printk("---      ex: echo 12 [SIZE] > /proc/kmsg_test ---\n");
	printk("=============== msg_layer usage pattern  ===============\n");
	printk("---  cat: showing msg_layer usage pattern ---\n");
	printk("---      ex: cat /proc/kmsg_test ---\n");
	printk("\n\n\n\n\n\n\n\n");

	return 0;
}

static void __exit msg_test_exit(void) 
{
	printk("\n\n--- Popcorn messaging self testing unloaded! ---\n\n");
	if(!g_test_buf)
		kfree(g_test_buf);
	remove_proc_entry("kmsg_test", NULL);
}

module_init(msg_test_init);
module_exit(msg_test_exit);
MODULE_LICENSE("GPL");
