/**
 * Header file for Popcorn inter-kernel messaging layer
 *
 * (C) Ben Shelton   <beshelto@vt.edu> 2013
 *     Sang-Hoon Kim <sanghoon@vt.edu> 2017-2018
 */

#ifndef __POPCORN_PCN_KMSG_H__
#define __POPCORN_PCN_KMSG_H__

#include <linux/types.h>
#include <linux/seq_file.h>

/* Enumerate message types */
enum pcn_kmsg_type {
	/* Thread migration */
	PCN_KMSG_TYPE_NODE_INFO,
	PCN_KMSG_TYPE_STAT_START,
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
	PCN_KMSG_TYPE_PAGE_INVALIDATE_REQUEST,
	PCN_KMSG_TYPE_PAGE_INVALIDATE_RESPONSE,
	PCN_KMSG_TYPE_REMOTE_PAGE_FLUSH,	/* XXX page flush is not working now */
	PCN_KMSG_TYPE_REMOTE_PAGE_RELEASE,
	PCN_KMSG_TYPE_REMOTE_PAGE_FLUSH_ACK,

	/* Distributed futex */
	PCN_KMSG_TYPE_FUTEX_REQUEST,
	PCN_KMSG_TYPE_FUTEX_RESPONSE,
	PCN_KMSG_TYPE_STAT_END,

	/* Performance experiments */
	PCN_KMSG_TYPE_TEST_REQUEST,
	PCN_KMSG_TYPE_TEST_RESPONSE,
	PCN_KMSG_TYPE_TEST_RDMA_REQUEST,
	PCN_KMSG_TYPE_TEST_RDMA_RESPONSE,

	/* Provide the single system image */
	PCN_KMSG_TYPE_REMOTE_PROC_CPUINFO_REQUEST,
	PCN_KMSG_TYPE_REMOTE_PROC_CPUINFO_RESPONSE,
	PCN_KMSG_TYPE_REMOTE_PROC_MEMINFO_REQUEST,
	PCN_KMSG_TYPE_REMOTE_PROC_MEMINFO_RESPONSE,
	PCN_KMSG_TYPE_REMOTE_PROC_PS_REQUEST,
	PCN_KMSG_TYPE_REMOTE_PROC_PS_RESPONSE,

	/* Schedule server */
	PCN_KMSG_TYPE_SCHED_PERIODIC,		/* XXX sched requires help!! */

	/* Popcorn socket redirection */
	PCN_KMSG_TYPE_REMOTE_SOCKET,
	PCN_KMSG_TYPE_REMOTE_SETSOCKOPT,
	PCN_KMSG_TYPE_REMOTE_SOCKET_RESPONSE,
	PCN_KMSG_TYPE_REMOTE_BIND,
	PCN_KMSG_TYPE_REMOTE_LISTEN,
	PCN_KMSG_TYPE_REMOTE_ACCEPT4,

	PCN_KMSG_TYPE_SYSCALL_FWD,
	PCN_KMSG_TYPE_SYSCALL_REP,
	PCN_KMSG_TYPE_SIGNAL_FWD,

	/* Popcorn MVX */
	PCN_KMSG_TYPE_MVX,		/* 41 - 43*/
	PCN_KMSG_TYPE_MVX_MSG,
	PCN_KMSG_TYPE_MVX_REPLY,

	PCN_KMSG_TYPE_MAX
};

/* Enumerate message priority. XXX Priority is not supported yet. */
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
	size_t size;
} __attribute__((packed));

#define PCN_KMSG_FROM_NID(x) \
	(((struct pcn_kmsg_message *)x)->header.from_nid)
#define PCN_KMSG_SIZE(x) (sizeof(struct pcn_kmsg_hdr) + x)

#define PCN_KMSG_MAX_SIZE (64UL << 10)
#define PCN_KMSG_MAX_PAYLOAD_SIZE \
	(PCN_KMSG_MAX_SIZE - sizeof(struct pcn_kmsg_hdr))


#define DEFINE_PCN_KMSG(type, fields) \
	typedef struct {				\
		struct pcn_kmsg_hdr header;	\
		fields;				\
	} __attribute__((packed)) type

struct pcn_kmsg_message {
	struct pcn_kmsg_hdr header;
	unsigned char payload[PCN_KMSG_MAX_PAYLOAD_SIZE];
} __attribute__((packed));

void pcn_kmsg_dump(struct pcn_kmsg_message *msg);


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
 * @msg is sent synchronously; it is safe to deallocate @msg after the return.
 */
int pcn_kmsg_send(enum pcn_kmsg_type type, int dest_nid, void *msg, size_t msg_size);

/**
 * Post @msg whose size is @msg_size to be sent to the node @dest_nid.
 * The message should be allocated through pcn_kmsg_get(), and the message
 * is reclaimed automatically once it is sent.
 */
int pcn_kmsg_post(enum pcn_kmsg_type type, int dest_nid, void *msg, size_t msg_size);

/**
 * Get message buffer for posting. Note pcn_kmsg_put() is for returning
 * unused buffer without posting it; posted message is reclaimed automatically.
 */
void *pcn_kmsg_get(size_t size);
void pcn_kmsg_put(void *msg);

/**
 * Process the received messag @msg. Each message layer should start processing
 * the request by calling this function
 */
void pcn_kmsg_process(struct pcn_kmsg_message *msg);

/**
 * Return received message @msg after handling to recyle it. @msg becomes
 * unavailable after the call. Make sure return received messages otherwise
 * the message layer will panick.
 */
void pcn_kmsg_done(void *msg);

/**
 * Print out transport-specific statistics into @buffer
 */
void pcn_kmsg_stat(struct seq_file *seq, void *v);


struct pcn_kmsg_rdma_handle {
	u32 rkey;
	void *addr;
	dma_addr_t dma_addr;
	void *private;
};

/**
 * Pin @buffer for RDMA and get @rdma_addr and @rdma_key.
 */
struct pcn_kmsg_rdma_handle *pcn_kmsg_pin_rdma_buffer(void *buffer, size_t size);

void pcn_kmsg_unpin_rdma_buffer(struct pcn_kmsg_rdma_handle *handle);

int pcn_kmsg_rdma_write(int dest_nid, dma_addr_t rdma_addr, void *addr, size_t size, u32 rdma_key);

int pcn_kmsg_rdma_read(int from_nid, void *addr, dma_addr_t rdma_addr, size_t size, u32 rdma_key);

/* TRANSPORT DESCRIPTOR */
enum {
	PCN_KMSG_FEATURE_RDMA = 1,
};

/**
 * Check the features that the transport layer provides. Return true iff all
 * features are supported.
 */
bool pcn_kmsg_has_features(unsigned int features);

struct pcn_kmsg_transport {
	char *name;
	unsigned long features;

	struct pcn_kmsg_message *(*get)(size_t);
	void (*put)(struct pcn_kmsg_message *);

	int (*send)(int, struct pcn_kmsg_message *, size_t);
	int (*post)(int, struct pcn_kmsg_message *, size_t);
	void (*done)(struct pcn_kmsg_message *);

	void (*stat)(struct seq_file *, void *);

	struct pcn_kmsg_rdma_handle *(*pin_rdma_buffer)(void *, size_t);
	void (*unpin_rdma_buffer)(struct pcn_kmsg_rdma_handle *);
	int (*rdma_write)(int, dma_addr_t, void *, size_t, u32);
	int (*rdma_read)(int, void *, dma_addr_t, size_t, u32);
};

void pcn_kmsg_set_transport(struct pcn_kmsg_transport *tr);

#endif /* __POPCORN_PCN_KMSG_H__ */
