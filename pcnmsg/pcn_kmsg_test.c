/*
 * Tests/benchmarks for Popcorn inter-kernel messaging layer
 *
 * (C) Ben Shelton <beshelto@vt.edu> 2013
 */

#include <linux/syscalls.h>
#include <linux/slab.h>

#include <popcorn/pcn_kmsg.h>

#define KMSG_TEST_VERBOSE 0

#if KMSG_TEST_VERBOSE
#define TEST_PRINTK(fmt, args...) printk("%s: " fmt, __func__, ##args)
#else
#define TEST_PRINTK(...) ;
#endif

#define TEST_ERR(fmt, args...) printk("%s: ERROR: " fmt, __func__, ##args)

/* INFRASTRUCTURE */
enum pcn_kmsg_test_op {
	PCN_KMSG_TEST_SEND_SINGLE,
	PCN_KMSG_TEST_SEND_PINGPONG,
	PCN_KMSG_TEST_SEND_BATCH,
	PCN_KMSG_TEST_SEND_BATCH_RESULT,
	PCN_KMSG_TEST_SEND_LONG,
	PCN_KMSG_TEST_OP_MCAST_OPEN,
	PCN_KMSG_TEST_OP_MCAST_SEND,
	PCN_KMSG_TEST_OP_MCAST_CLOSE
};

struct pcn_kmsg_test_args {
	int cpu;
	unsigned long mask;
	unsigned long batch_size;
	pcn_kmsg_mcast_id mcast_id;
	unsigned long send_ts;
	unsigned long ts0;
	unsigned long ts1;
	unsigned long ts2;
	unsigned long ts3;
	unsigned long ts4;
	unsigned long ts5;
	unsigned long rtt;
};

/* MESSAGE TYPES */
/* Message struct for testing */
struct pcn_kmsg_test_message {
	struct pcn_kmsg_hdr header;
	enum pcn_kmsg_test_op op;
	unsigned long batch_seqnum;
	unsigned long batch_size;
	unsigned long ts1, ts2, ts3, ts4, ts5;
	//char pad[16];
}__attribute__((packed)) __attribute__((aligned(64)));


static inline uint32_t rdtsc32(void)
{
//#if defined(__GNUC__) && defined(__ARM_ARCH_7A__)
#if defined(CONFIG_ARM64)
    //uint32_t r = 0;
    uint32_t val = 0;
    //asm volatile("mrc p15, 0, %0, c9, c13, 0" : "=r"(r) );
    asm volatile("msr cntv_ctl_el0,  %0" : : "r" (val));
    return val;
#else
//#error Unsupported architecture/compiler!
	return -EINVAL;
#endif
}


volatile unsigned long kmsg_tsc;
unsigned long ts1, ts2, ts3, ts4, ts5;

volatile int kmsg_done;

int my_cpu;
volatile unsigned long isr_ts, isr_ts_2, bh_ts, bh_ts_2;

static int pcn_kmsg_test_send_single(struct pcn_kmsg_test_args __user *args)
{
	int rc = 0;
	struct pcn_kmsg_test_message msg;
	unsigned long ts_start, ts_end;

	msg.header.type = PCN_KMSG_TYPE_TEST;
	msg.header.prio = PCN_KMSG_PRIO_HIGH;
	msg.op = PCN_KMSG_TEST_SEND_SINGLE;

#if defined(CONFIG_ARM64)
    ts_start = rdtsc32();
#elif defined(CONFIG_X86_64)
    rdtscll(ts_start);
#endif

	pcn_kmsg_send(args->cpu, &msg, sizeof(msg));

#if defined(CONFIG_ARM64)
    ts_end = rdtsc32();
#elif defined(CONFIG_X86_64)
	rdtscll(ts_end);
#endif

	args->send_ts = ts_end - ts_start;

	return rc;
}

unsigned long int_ts;

static int pcn_kmsg_test_send_pingpong(struct pcn_kmsg_test_args __user *args)
{
	int rc = 0;
	struct pcn_kmsg_test_message msg;
	unsigned long tsc_init;

	msg.header.type = PCN_KMSG_TYPE_TEST;
	msg.header.prio = PCN_KMSG_PRIO_HIGH;
	msg.op = PCN_KMSG_TEST_SEND_PINGPONG;

	kmsg_done = 0;

#if defined(CONFIG_ARM64)
    tsc_init = rdtsc32();
#elif defined(CONFIG_X86_64)
    rdtscll(tsc_init);
#endif

    pcn_kmsg_send(args->cpu, &msg, sizeof(msg));
	while (!kmsg_done) {}

	TEST_PRINTK("Elapsed time (ticks): %lu\n", kmsg_tsc - tsc_init);

	args->send_ts = tsc_init;
	args->ts0 = int_ts;
	args->ts1 = ts1;
	args->ts2 = ts2;
	args->ts3 = ts3;
	args->ts4 = ts4;
	args->ts5 = ts5;
	args->rtt = kmsg_tsc;

	return rc;
}

static int pcn_kmsg_test_send_batch(struct pcn_kmsg_test_args __user *args)
{
	int rc = 0;
	unsigned long i;
	struct pcn_kmsg_test_message msg;
	unsigned long batch_send_start_tsc, batch_send_end_tsc;

	TEST_PRINTK("Testing batch send, batch_size %lu\n", args->batch_size);

	msg.header.type = PCN_KMSG_TYPE_TEST;
	msg.header.prio = PCN_KMSG_PRIO_HIGH;
	msg.op = PCN_KMSG_TEST_SEND_BATCH;
	msg.batch_size = args->batch_size;

#if defined(CONFIG_ARM64)
    batch_send_start_tsc = rdtsc32();
#elif defined(CONFIG_X86_64)
    rdtscll(batch_send_start_tsc);
#endif

	kmsg_done = 0;

	/* send messages in series */
	for (i = 0; i < args->batch_size; i++) {
		msg.batch_seqnum = i;

		TEST_PRINTK("Sending batch message, cpu %d, seqnum %lu\n",
			    args->cpu, i);

		rc = pcn_kmsg_send(args->cpu, &msg, sizeof(msg));

		if (rc) {
			TEST_ERR("Error sending message!\n");
			return -1;
		}
	}

#if defined(CONFIG_ARM64)
    batch_send_end_tsc = rdtsc32();
#elif defined(CONFIG_X86_64)
    rdtscll(batch_send_end_tsc);
#endif


	/* wait for reply to last message */

	while (!kmsg_done) {}

	args->send_ts = batch_send_start_tsc;
	args->ts0 = batch_send_end_tsc;
	args->ts1 = ts1;
	args->ts2 = ts2;
	args->ts3 = ts3;
	args->rtt = kmsg_tsc;

	return rc;
}


const char *__long_str = "This is a very long test message.  Don't be surprised if it gets corrupted; it probably will.  If it does, you're in for a lot more work, and may not get home to see your wife this weekend.  You should knock on wood before running this test.";

static int pcn_kmsg_test_long_msg(struct pcn_kmsg_test_args __user *args)
{
	int rc;
	unsigned long start_ts, end_ts;
	struct pcn_kmsg_message *lmsg = kzalloc(sizeof(*lmsg), GFP_KERNEL);;

	lmsg->header.type = PCN_KMSG_TYPE_TEST_LONG;
	lmsg->header.prio = PCN_KMSG_PRIO_NORMAL;

	strcpy((char *) &lmsg->payload, __long_str);

	TEST_PRINTK("Message to send: %s\n", (char *) &lmsg->payload);

	TEST_PRINTK("syscall to test kernel messaging, to CPU %d\n",
		    args->cpu);

#if defined(CONFIG_ARM64)
    start_ts = rdtsc32();
#elif defined(CONFIG_X86_64)
    rdtscll(start_ts);
#endif

	rc = pcn_kmsg_send(args->cpu, lmsg, strlen(__long_str) + 5);

#if defined(CONFIG_ARM64)
    end_ts = rdtsc32();
#elif defined(CONFIG_X86_64)
    rdtscll(end_ts);
#endif

	args->send_ts = end_ts - start_ts;

	TEST_PRINTK("POPCORN: pcn_kmsg_send returned %d\n", rc);
	kfree(lmsg);

	return rc;
}

#ifdef PCN_SUPPORT_MULTICAST
static int pcn_kmsg_test_mcast_open(struct pcn_kmsg_test_args __user *args)
{
	int rc;
	pcn_kmsg_mcast_id test_id = -1;

	/* open */
	TEST_PRINTK("open\n");
	rc = pcn_kmsg_mcast_open(&test_id, args->mask);

	TEST_PRINTK("pcn_kmsg_mcast_open returned %d, test_id %lu\n",
		    rc, test_id);

	args->mcast_id = test_id;

	return rc;
}

extern unsigned long mcast_ipi_ts;

static int pcn_kmsg_test_mcast_send(struct pcn_kmsg_test_args __user *args)
{
	int rc;
	struct pcn_kmsg_test_message msg;
	unsigned long ts_start, ts_end;

	/* send */
	TEST_PRINTK("send\n");
	msg.header.type = PCN_KMSG_TYPE_TEST;
	msg.header.prio = PCN_KMSG_PRIO_HIGH;
	msg.op = PCN_KMSG_TEST_SEND_SINGLE;

#if defined(CONFIG_ARM64)
    ts_start = rdtsc32();
#elif defined(CONFIG_X86_64)
    rdtscll(ts_start);
#endif

	rc = pcn_kmsg_mcast_send(args->mcast_id,
				 (struct pcn_kmsg_message *) &msg);

#if defined(CONFIG_ARM64)
    ts_end = rdtsc32();
#elif defined(CONFIG_X86_64)
    rdtscll(ts_end);
#endif

	if (rc) {
		TEST_ERR("failed to send mcast message to group %lu!\n",
			 args->mcast_id);
	}

	args->send_ts = ts_start;
	args->ts0 = mcast_ipi_ts;
	args->ts1 = ts_end;

	return rc;
}

static int pcn_kmsg_test_mcast_close(struct pcn_kmsg_test_args __user *args)
{
	int rc;

	/* close */
	TEST_PRINTK("close\n");

	rc = pcn_kmsg_mcast_close(args->mcast_id);

	TEST_PRINTK("mcast close returned %d\n", rc);

	return rc;
}
#endif /* PCN_SUPPORT_MULTICAST */

/* Syscall for testing all this stuff */
SYSCALL_DEFINE2(popcorn_test_kmsg, enum pcn_kmsg_test_op, op,
		struct pcn_kmsg_test_args __user *, args)
{
	int rc = 0;

	TEST_PRINTK("Reached test kmsg syscall, op %d, cpu %d\n",
		    op, args->cpu);

	switch (op) {
		case PCN_KMSG_TEST_SEND_SINGLE:
			rc = pcn_kmsg_test_send_single(args);
			break;

		case PCN_KMSG_TEST_SEND_PINGPONG:
			rc = pcn_kmsg_test_send_pingpong(args);
			break;

		case PCN_KMSG_TEST_SEND_BATCH:
			rc = pcn_kmsg_test_send_batch(args);
			break;

		case PCN_KMSG_TEST_SEND_LONG:
			rc = pcn_kmsg_test_long_msg(args);
			break;
#ifdef PCN_SUPPORT_MULTICAST
		case PCN_KMSG_TEST_OP_MCAST_OPEN:
			rc = pcn_kmsg_test_mcast_open(args);
			break;

		case PCN_KMSG_TEST_OP_MCAST_SEND:
			rc = pcn_kmsg_test_mcast_send(args);
			break;

		case PCN_KMSG_TEST_OP_MCAST_CLOSE:
			rc = pcn_kmsg_test_mcast_close(args);
			break;
#endif /* PCN_SUPPORT_MULTICAST */

		default:
			TEST_ERR("invalid option %d\n", op);
			return -1;
	}

	return rc;
}

/* CALLBACKS */

static int handle_single_msg(struct pcn_kmsg_test_message *msg)
{
	TEST_PRINTK("Received single test message from CPU %d!\n",
		    msg->header.from_nid);
	return 0;
}

static int handle_pingpong_msg(struct pcn_kmsg_test_message *msg)
{
	int rc = 0;
	unsigned long handler_ts;

#if defined(CONFIG_ARM64)
    handler_ts = rdtsc32();
#elif defined(CONFIG_X86_64)
    rdtscll(handler_ts);
#endif

	if (my_cpu) {

		struct pcn_kmsg_test_message reply_msg;

		reply_msg.header.type = PCN_KMSG_TYPE_TEST;
		reply_msg.header.prio = PCN_KMSG_PRIO_HIGH;
		reply_msg.op = PCN_KMSG_TEST_SEND_PINGPONG;
		reply_msg.ts1 = isr_ts;
		reply_msg.ts2 = isr_ts_2;
		reply_msg.ts3 = bh_ts;
		reply_msg.ts4 = bh_ts_2;
		reply_msg.ts5 = handler_ts;

		TEST_PRINTK("Sending message back to CPU 0...\n");
		rc = pcn_kmsg_send(0, &reply_msg, sizeof(reply_msg));

		if (rc) {
			TEST_ERR("Message send failed!\n");
			return -1;
		}

		isr_ts = isr_ts_2 = bh_ts = bh_ts_2 = 0;
	} else {
		TEST_PRINTK("Received ping-pong; reading end timestamp...\n");
#if defined(CONFIG_ARM64)
        kmsg_tsc = rdtsc32();
#elif defined(CONFIG_X86_64)
        rdtscll(kmsg_tsc);
#endif

        ts1 = msg->ts1;
		ts2 = msg->ts2;
		ts3 = msg->ts3;
		ts4 = msg->ts4;
		ts5 = msg->ts5;
		kmsg_done = 1;
	}

	return 0;
}

unsigned long batch_start_tsc;

static int handle_batch_msg(struct pcn_kmsg_test_message *msg)
{
	int rc = 0;

	TEST_PRINTK("seqnum %lu size %lu\n", msg->batch_seqnum,
		    msg->batch_size);

	if (msg->batch_seqnum == 0) {
		TEST_PRINTK("Start of batch; taking initial timestamp!\n");
#if defined(CONFIG_ARM64)
        batch_start_tsc = rdtsc32();
#elif defined(CONFIG_X86_64)
        rdtscll(batch_start_tsc);
#endif

	}

	if (msg->batch_seqnum == (msg->batch_size - 1)) {
		/* send back reply */
		struct pcn_kmsg_test_message reply_msg;
		unsigned long batch_end_tsc;

		TEST_PRINTK("End of batch; sending back reply!\n");
#if defined(CONFIG_ARM64)
        batch_end_tsc = rdtsc32();
#elif defined(CONFIG_X86_64)
        rdtscll(batch_end_tsc);
#endif

		reply_msg.header.type = PCN_KMSG_TYPE_TEST;
		reply_msg.header.prio = PCN_KMSG_PRIO_HIGH;
		reply_msg.op = PCN_KMSG_TEST_SEND_BATCH_RESULT;
		reply_msg.ts1 = bh_ts;
		reply_msg.ts2 = bh_ts_2;
		reply_msg.ts3 = batch_end_tsc;

		isr_ts = isr_ts_2 = bh_ts = bh_ts_2 = 0;

		rc = pcn_kmsg_send(0, &reply_msg, sizeof(reply_msg));

		if (rc) {
			TEST_ERR("Message send failed!\n");
			return -1;
		}
	}
	return rc;
}

static int handle_batch_result_msg(struct pcn_kmsg_test_message *msg)
{
	int rc = 0;

#if defined(CONFIG_ARM64)
    kmsg_tsc = rdtsc32();
#elif defined(CONFIG_X86_64)
    rdtscll(kmsg_tsc);
#endif

	ts1 = msg->ts1;
	ts2 = msg->ts2;
	ts3 = msg->ts3;

	kmsg_done = 1;

	return rc;
}

static int pcn_kmsg_test_callback(struct pcn_kmsg_message *message)
{
	int rc = 0;

	struct pcn_kmsg_test_message *msg =
		(struct pcn_kmsg_test_message *) message;

	TEST_PRINTK("Reached %s, op %d!\n", __func__, msg->op);

	switch (msg->op) {
		case PCN_KMSG_TEST_SEND_SINGLE:
			rc = handle_single_msg(msg);
			break;

		case PCN_KMSG_TEST_SEND_PINGPONG:
			rc = handle_pingpong_msg(msg);
			break;

		case PCN_KMSG_TEST_SEND_BATCH:
			rc = handle_batch_msg(msg);
			break;

		case PCN_KMSG_TEST_SEND_BATCH_RESULT:
			rc = handle_batch_result_msg(msg);
			break;

		default:
			TEST_ERR("Operation %d not supported!\n", msg->op);
	}

	pcn_kmsg_free_msg(message);

	return rc;
}

static int pcn_kmsg_test_long_callback(struct pcn_kmsg_message *message)
{
	struct pcn_kmsg_message *lmsg =
		(struct pcn_kmsg_message *)message;

	TEST_PRINTK("Received test long message, payload: %s\n",
		    (char *) &lmsg->payload);

	pcn_kmsg_free_msg(lmsg);

	return 0;
}


static int __init pcn_kmsg_test_init(void)
{
	int rc;

	TEST_PRINTK("Registering test callbacks!\n");

	rc = pcn_kmsg_register_callback(PCN_KMSG_TYPE_TEST,
					pcn_kmsg_test_callback);
	if (rc) {
		TEST_ERR("Failed to register initial kmsg test callback!\n");
	}

	rc = pcn_kmsg_register_callback(PCN_KMSG_TYPE_TEST_LONG,
					pcn_kmsg_test_long_callback);
	if (rc) {
		TEST_ERR("Failed to register initial kmsg_test_long callback!\n");
	}

	return rc;
}

late_initcall(pcn_kmsg_test_init);
