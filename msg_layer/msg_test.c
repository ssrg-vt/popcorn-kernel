/*
 * Copyright (C) 2017 JackChuang <horenc@vt.edu>
 * 
 * Testing msg_layer    - general send recv
 *                      - large data (>=8K)
 *                      - (rdma read/write)
 */
#include <linux/module.h>

#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/kthread.h>

#include <linux/string.h>

#include <asm/uaccess.h>

#include <popcorn/debug.h>
#include <popcorn/pcn_kmsg.h>

#include "common.h"

#define ITER 1
#define MAX_ARGS_NUM 100
#define MAX_MSG_LENGTH 16384
#define MSG_COUNT 100000

/* getting performance data */
#define POPCORN_EXP_DATA_MSG_IB 1
#if POPCORN_EXP_DATA_MSG_IB
#define EXP_DATA(...) printk(__VA_ARGS__)
#else
#define EXP_DATA(...)
#endif 

/* making sure it's done */
#define POPCORN_EXP_LOG_MSG_IB 1
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
char *g_test_buf;				// mimicing user buf

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

struct test_msg_t
{
	struct pcn_kmsg_hdr header;
	unsigned char payload[MAX_MSG_LENGTH];
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

	DEBUG_LOG_V("%s(): %s\n", __func__, request->payload);

	pcn_kmsg_free_msg(request);
	return 0;
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
		pcn_kmsg_send_long(i, (struct pcn_kmsg_long_message*) request,
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

// only support nid x sends to nid 0 so far
void test_send_throughput(unsigned long long payload_size)
{
	int i;
	struct timeval t1, t2;
	struct test_msg_t *msg;

	msg = pcn_kmsg_alloc_msg(sizeof(struct test_msg_t));
	msg->header.type= PCN_KMSG_TYPE_SELFIE_TEST;
	memset(msg->payload, 'b', payload_size);

	if (my_nid == 0) {
		DEBUG_LOG_V("%s: my_nid is %d, Skip! \n",__func__, my_nid);
		return;
	}

	do_gettimeofday(&t1);
	for (i = 0; i < MAX_TESTING_SIZE/payload_size; i++)
		pcn_kmsg_send_long(0, msg, payload_size + sizeof(msg->header));
	do_gettimeofday(&t2);
	
	if( t2.tv_usec-t1.tv_usec >= 0) {
		EXP_DATA("Send throughput result: send payload size %llu, "
							"total size %d, %llu times, spent %ld.%06ld s\n",
				payload_size, MAX_TESTING_SIZE, MAX_TESTING_SIZE/payload_size, 
									t2.tv_sec-t1.tv_sec, t2.tv_usec-t1.tv_usec);
	} else {
		EXP_DATA("Send throughput result: send payload size %llu, "
							"total size %d, %llu times, spent %ld.%06ld s\n",
				payload_size, MAX_TESTING_SIZE, MAX_TESTING_SIZE/payload_size,
					t2.tv_sec-t1.tv_sec-1, (1000000-(t1.tv_usec-t2.tv_usec)));
	}

	pcn_kmsg_free_msg(msg);
}

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
		while(++cnt<=MSG_COUNT)
			test1();
		do_gettimeofday(&t2);
		if( t2.tv_usec-t1.tv_usec >= 0) {
			EXP_DATA("Send throughput result: send msg size %d, "
						"total size %d, %d times, spent %ld.%06ld s\n",
							TEST1_PAYLOAD_SIZE, MAX_TESTING_SIZE, MSG_COUNT, 
								t2.tv_sec-t1.tv_sec, t2.tv_usec-t1.tv_usec);
		} else {
			EXP_DATA("Send throughput result: size %d, "
						"total size %d, %d times, spent %ld.%06ld s\n",
						TEST1_PAYLOAD_SIZE, MAX_TESTING_SIZE, MSG_COUNT,
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

		KRPRINT_INIT("arg %llu\n", simple_strtoull(argv[1], NULL, 0));
		for(i=0; i<ITER; i++) {
			do_gettimeofday(&t1);
			test_send_throughput(simple_strtoull(argv[1], NULL, 0));
			do_gettimeofday(&t2);
#if 0			
			if( t2.tv_usec-t1.tv_usec >= 0) {
				EXP_DATA("Send throughput result: send msg size %llu, "
							"total size %llu, %llu times, spent %ld.%06ld s\n",
				payload_size, MAX_TESTING_SIZE, MAX_TESTING_SIZE/payload_size, 
									t2.tv_sec-t1.tv_sec, t2.tv_usec-t1.tv_usec);
			} else {
				EXP_DATA("Send throughput result: size %llu, "
							"total size %llu, %llu times, spent %ld.%06ld s\n",
				payload_size, MAX_TESTING_SIZE, MAX_TESTING_SIZE/payload_size,
					t2.tv_sec-t1.tv_sec-1, (1000000-(t1.tv_usec-t2.tv_usec)));
			}
#endif
		}
		EXP_LOG("test %c%c test_send_throughput() done\n\n\n", 
														cmd[0], cmd[1]);
	}
	else { 
		printk("Not support yet. Try \"1,2,3,4,5,6,9,10\"\n"); 
	}

	kfree(cmd);
	module_put(THIS_MODULE);
	KRPRINT_INIT("proc write done!!\n");
	return count;	// if not reach count, will reenter again
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
	.read = seq_read,
	.llseek  = seq_lseek,
	.release = single_release,
	.write = write_proc,
};


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

	smp_mb(); // Just in case
	printk("--- Popcorn messaging layer self-testing proc init done ---\n");
	printk("--- Usage: sudo echo [NUM] [DEPENDS] > /proc/kmsg_test ---\n");
	printk("------------------ sanity check ---------------------\n");
	printk("---  1: continuously send/recv test ---\n");
	printk("---  2: continuously READ test ---\n");
	printk("---  3: continuously WRITE test ---\n");
	printk("---  4: continuously multithreading send/recv test ---\n");
	printk("---  5: continuously multithreading READ test ---\n");
	printk("---  6: continuously multithreading WRITE test ---\n");
	printk("---  9: continuously multithreading "
									"send/recv/READ/WRITE test ---\n\n");
	printk("---      ex: echo [NUM] > /proc/kmsg_test---\n");
	printk("---------------- experimental data -------------------\n");
	printk("---  10: send throughput ---\n");
	printk("---      ex: echo 10 [SIZE] > /proc/kmsg_test---\n");
	printk("---  11: send throughput ---\n");
	printk("---      ex: echo 11 > /proc/kmsg_test---\n");
	printk("\n\n\n\n\n\n\n\n");

	return 0;
}

static void __exit msg_test_exit(void) 
{
	printk("\n\n--- Popcorn messaging self testing unloaded! ---\n\n");
	remove_proc_entry("kmsg_test", NULL);
}

module_init(msg_test_init);
module_exit(msg_test_exit);
MODULE_LICENSE("GPL");
