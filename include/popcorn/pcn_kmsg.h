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
	PCN_KMSG_TYPE_TEST_REQUEST,
	PCN_KMSG_TYPE_TEST_RESPONSE,
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
	int from_nid			:6;
	enum pcn_kmsg_prio prio	:2;
	enum pcn_kmsg_type type	:8;
	int flags				:8;
	size_t size;
} __attribute__((packed));

#define PCN_KMSG_FROM_NID(x) \
	(((struct pcn_kmsg_message *)x)->header.from_nid)
#define PCN_KMSG_SIZE(x) (sizeof(struct pcn_kmsg_hdr) + x)

/* rdma header */
struct pcn_kmsg_rdma_hdr {
    bool rdma_ack			:1;
    bool is_write			:1;
    enum pcn_kmsg_type rmda_type_res	:6;	/* response callback func */
    uint32_t remote_rkey;
    size_t rw_size;
    uint64_t remote_addr;
    void *your_buf_ptr;			/* will be copied to R/W buffer */
} __attribute__((packed));

#define PCN_KMSG_MAX_SIZE (64UL << 10)
#define PCN_KMSG_MAX_PAYLOAD_SIZE (PCN_KMSG_MAX_SIZE \
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
	unsigned char payload[PCN_KMSG_MAX_PAYLOAD_SIZE];
} __attribute__((packed));

/* SETUP */

/* Function pointer to callback functions */
typedef int (*pcn_kmsg_cbftn)(struct pcn_kmsg_message *);

/* Register a callback function to handle the message type */
int pcn_kmsg_register_callback(enum pcn_kmsg_type type, pcn_kmsg_cbftn callback);

/* Unregister a callback function for the message type */
int pcn_kmsg_unregister_callback(enum pcn_kmsg_type type);


/* MESSAGING */
/**
 * Send @msg whose size is @msg_size to the node @dest_nid.
 * @msg is sent synchronously
 */
int pcn_kmsg_send(enum pcn_kmsg_type type, int dest_nid, void *msg, size_t msg_size);

/**
 * Post @msg whose size is @msg_size to be sent to the node @dest_nid.
 * The messsage is sent asynchronously
 */
int pcn_kmsg_post(enum pcn_kmsg_type type, int dest_nid, void *msg, size_t msg_size);


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
    
void *pcn_kmsg_request_rdma(enum pcn_kmsg_type type, int dest_nid, void *msg, size_t msg_size, size_t rw_size);

void pcn_kmsg_respond_rdma(enum pcn_kmsg_type type, void *msg, void *paddr, size_t rw_size);


/* Allocate/free buffers for receiving a message */
void *pcn_kmsg_get(size_t size);
void pcn_kmsg_put(void *msg);
void pcn_kmsg_done(void *msg);


enum pcn_kmsg_layer_types {
	PCN_KMSG_LAYER_TYPE_UNKNOWN = -1,
	PCN_KMSG_LAYER_TYPE_SOCKET = 0,
	PCN_KMSG_LAYER_TYPE_RDMA,
	PCN_KMSG_LAYER_TYPE_IB,
	PCN_KMSG_LAYER_TYPE_DOLPHIN,
	PCN_KMSG_LAYER_TYPE_MAX,
};

typedef int (*send_ftn)(int, struct pcn_kmsg_message *, size_t);
typedef int (*post_ftn)(int, struct pcn_kmsg_message *, size_t);

typedef struct pcn_kmsg_message *(*get_ftn)(size_t);
typedef void (*put_ftn)(struct pcn_kmsg_message *);
typedef void (*done_ftn)(struct pcn_kmsg_message *);

typedef void* (*request_rdma_ftn)(int, pcn_kmsg_rdma_t *, size_t, size_t);
typedef void (*respond_rdma_ftn)(pcn_kmsg_rdma_t *, void *, size_t rw_size);

struct pcn_kmsg_transport {
	char *name;
	enum pcn_kmsg_layer_types type;

	send_ftn send_fn;
	post_ftn post_fn;
	done_ftn done_fn;

	get_ftn get_fn;
	put_ftn put_fn;

	request_rdma_ftn request_rdma_fn;
	respond_rdma_ftn respond_rdma_fn;
};

extern void pcn_kmsg_set_transport(struct pcn_kmsg_transport *tr);

#endif /* __LINUX_PCN_KMSG_H */
