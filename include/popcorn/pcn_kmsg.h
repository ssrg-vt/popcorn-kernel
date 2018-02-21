/*
 * Header file for Popcorn inter-kernel messaging layer
 *
 * (C) Ben Shelton <beshelto@vt.edu> 2013
 */

#ifndef __LINUX_PCN_KMSG_H
#define __LINUX_PCN_KMSG_H

#include <linux/types.h>

/* Enum for message types */
enum pcn_kmsg_type {
	/* RDMA handlers */
	PCN_KMSG_TYPE_RDMA_KEY_EXCHANGE_REQUEST,
	PCN_KMSG_TYPE_RDMA_KEY_EXCHANGE_RESPONSE,

	/* Performance experiments */
	PCN_KMSG_TYPE_TEST_0,
	PCN_KMSG_TYPE_TEST_1,
	PCN_KMSG_TYPE_SEND_ROUND_READ_REQUEST,
	PCN_KMSG_TYPE_SEND_ROUND_READ_RESPONSE,
	PCN_KMSG_TYPE_SEND_ROUND_WRITE_REQUEST,
	PCN_KMSG_TYPE_SEND_ROUND_WRITE_RESPONSE,
	PCN_KMSG_TYPE_SHOW_REMOTE_TEST_BUF,
	PCN_KMSG_TYPE_RDMA_READ_TEST_REQUEST,
	PCN_KMSG_TYPE_RDMA_READ_TEST_RESPONSE,
	PCN_KMSG_TYPE_RDMA_WRITE_TEST_REQUEST,
	PCN_KMSG_TYPE_RDMA_WRITE_TEST_RESPONSE,

	/* Provide the single system image */
	PCN_KMSG_TYPE_REMOTE_PROC_CPUINFO_REQUEST,
	PCN_KMSG_TYPE_REMOTE_PROC_CPUINFO_RESPONSE,
	PCN_KMSG_TYPE_REMOTE_PROC_MEMINFO_REQUEST,
	PCN_KMSG_TYPE_REMOTE_PROC_MEMINFO_RESPONSE,
	PCN_KMSG_TYPE_REMOTE_PROC_PS_REQUEST,
	PCN_KMSG_TYPE_REMOTE_PROC_PS_RESPONSE,

	/* Thread migration */
	PCN_KMSG_TYPE_NODE_INFO,
	PCN_KMSG_TYPE_TASK_MIGRATE,
	PCN_KMSG_TYPE_TASK_MIGRATE_BACK,
	PCN_KMSG_TYPE_TASK_PAIRING,
	PCN_KMSG_TYPE_TASK_EXIT_ORIGIN,
	PCN_KMSG_TYPE_TASK_EXIT_REMOTE,

	/* VMA synchronization */
	PCN_KMSG_TYPE_VMA_INFO_REQUEST,
	PCN_KMSG_TYPE_VMA_INFO_RESPONSE,
	PCN_KMSG_TYPE_VMA_OP_REQUEST,
	PCN_KMSG_TYPE_VMA_OP_RESPONSE,

	/* Page consistency protocol */
	PCN_KMSG_TYPE_REMOTE_PAGE_REQUEST,
	PCN_KMSG_TYPE_REMOTE_PAGE_RESPONSE,
	PCN_KMSG_TYPE_REMOTE_PAGE_RESPONSE_SHORT,
	PCN_KMSG_TYPE_REMOTE_PAGE_FLUSH,
	PCN_KMSG_TYPE_REMOTE_PAGE_RELEASE,
	PCN_KMSG_TYPE_REMOTE_PAGE_FLUSH_ACK,
	PCN_KMSG_TYPE_PAGE_INVALIDATE_REQUEST,
	PCN_KMSG_TYPE_PAGE_INVALIDATE_RESPONSE,

	/* Distributed futex */
	PCN_KMSG_TYPE_FUTEX_REQUEST,
	PCN_KMSG_TYPE_FUTEX_RESPONSE,

	/* Schedule server */
	PCN_KMSG_TYPE_SCHED_PERIODIC,

	PCN_KMSG_TYPE_MAX
};

/* Enum for message priority */
enum pcn_kmsg_prio {
	PCN_KMSG_PRIO_LOW,
	PCN_KMSG_PRIO_NORMAL,
	PCN_KMSG_PRIO_HIGH,
};

/* Message header */
struct pcn_kmsg_hdr {
	unsigned int from_nid	:8;
	enum pcn_kmsg_type type	:8;
	enum pcn_kmsg_prio prio	:7;
	bool is_rdma			:1;
	size_t size;
} __attribute__((packed));

/* rdma header */
struct pcn_kmsg_rdma_hdr {
    bool rdma_ack			:1;
    bool is_write			:1;
    enum pcn_kmsg_type rmda_type_res	:6;	/* response callback func */
    uint32_t remote_rkey;
    uint32_t rw_size;
    uint64_t remote_addr;
    void *your_buf_ptr;			/* will be copied to R/W buffer */
} __attribute__((packed));

#define CACHE_LINE_SIZE 64
#define PCN_KMSG_MAX_SIZE_TOTAL (64UL << 10)
#define PCN_KMSG_MAX_SIZE  (PCN_KMSG_MAX_SIZE_TOTAL \
					- sizeof(struct pcn_kmsg_hdr) \
					- sizeof(struct pcn_kmsg_rdma_hdr))

#define DEFINE_PCN_KMSG(type, fields) \
	typedef struct {				\
		struct pcn_kmsg_hdr header;	\
		fields				\
	}__attribute__((packed)) type

#define DEFINE_PCN_RDMA_KMSG(type, fields) \
	typedef struct {				\
		struct pcn_kmsg_hdr header;	\
		struct pcn_kmsg_rdma_hdr rdma_header; \
		void *private; \
		fields				\
	}__attribute__((packed)) type

/* Struct for the actual messages. Note that hdr and payload are flipped
   when this actually goes out, so the receiver can poll on the ready bit
   in the header. */
struct pcn_kmsg_message {
	struct pcn_kmsg_hdr header;
	unsigned char payload[PCN_KMSG_MAX_SIZE];
} __attribute__((packed, aligned(CACHE_LINE_SIZE)));

/* SETUP */

/* Function pointer to callback functions */
typedef int (*pcn_kmsg_cbftn)(struct pcn_kmsg_message *);
extern pcn_kmsg_cbftn pcn_kmsg_cbftns[PCN_KMSG_TYPE_MAX];

/* Register a callback function to handle the message type */
int pcn_kmsg_register_callback(enum pcn_kmsg_type type, pcn_kmsg_cbftn callback);

/* Unregister a callback function for the message type */
int pcn_kmsg_unregister_callback(enum pcn_kmsg_type type);


/* MESSAGING */
/* Send @msg whose size is @msg_size to the node @dest_nid */
int pcn_kmsg_send(int dest_nid, void *msg, size_t msg_size);

typedef int (*send_ftn)(int, struct pcn_kmsg_message *, size_t);
extern send_ftn pcn_kmsg_send_ftn;

/* Post @msg whose size is @msg_size to be sent to the node @dest_nid */
int pcn_kmsg_post(int dest_nid, void *msg, size_t msg_size);

typedef int (*post_ftn)(int, struct pcn_kmsg_message *, size_t);
extern post_ftn pcn_kmsg_post_ftn;

/**
 * Process the received messag @msg. Each message layer should start processing
 * the request by calling this function
 */
void pcn_kmsg_process(struct pcn_kmsg_message *msg);


/**
 * RDMA-specific functions
 */
#define RDMA_TEMPLATE ;
DEFINE_PCN_RDMA_KMSG(pcn_kmsg_rdma_t, RDMA_TEMPLATE);
    
void *pcn_kmsg_request_rdma(int dest_nid, void *msg, size_t msg_size, size_t rw_size);

typedef void* (*request_rdma_ftn)(int, pcn_kmsg_rdma_t *, size_t, size_t);
extern request_rdma_ftn pcn_kmsg_request_rdma_ftn;

void pcn_kmsg_respond_rdma(void *msg, void *paddr, size_t rw_size);

typedef void (*respond_rdma_ftn)(pcn_kmsg_rdma_t *, void *, size_t rw_size);
extern respond_rdma_ftn pcn_kmsg_respond_rdma_ftn;


/* Allocate/free buffers for receiving a message */
void *pcn_kmsg_alloc_msg(size_t size);
void pcn_kmsg_free_msg(void *msg);

typedef void (*free_ftn)(struct pcn_kmsg_message *);
extern free_ftn pcn_kmsg_free_ftn;


enum pcn_kmsg_layer_types {
	PCN_KMSG_LAYER_TYPE_UNKNOWN = -1,
	PCN_KMSG_LAYER_TYPE_SOCKET = 0,
	PCN_KMSG_LAYER_TYPE_IB,
	PCN_KMSG_LAYER_TYPE_DOLPHIN,
	PCN_KMSG_LAYER_TYPE_MAX,
};
extern enum pcn_kmsg_layer_types pcn_kmsg_layer_type;

#endif /* __LINUX_PCN_KMSG_H */
