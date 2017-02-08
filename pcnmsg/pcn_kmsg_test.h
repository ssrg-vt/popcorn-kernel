#ifndef __LINUX_PCN_KMSG_TEST_H
#define __LINUX_PCN_KMSG_TEST_H
/*
 * Header file for Popcorn inter-kernel messaging layer
 *
 * (C) Ben Shelton <beshelto@vt.edu> 2013
 */


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

#endif /* __LINUX_PCN_KMSG_TEST_H */
