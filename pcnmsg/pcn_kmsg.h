#ifndef __LINUX_PCN_KMSG_H
#define __LINUX_PCN_KMSG_H
/*
 * Header file for Popcorn inter-kernel messaging layer
 *
 * (C) Ben Shelton <beshelto@vt.edu> 2013
 */

#include <linux/types.h>

enum pcn_connection_status{
	PCN_CONN_WATING,
	PCN_CONN_CONNECTED,
	PCN_CONN_TYPE_MAX
};
typedef unsigned long pcn_kmsg_mcast_id;
/* MESSAGING */

/* Enum for message types.  Modules should add types after
   PCN_KMSG_END. */
enum pcn_kmsg_type {
	PCN_KMSG_TYPE_TEST,
	PCN_KMSG_TYPE_TEST_LONG,
	PCN_KMSG_TYPE_CHECKIN,
	PCN_KMSG_TYPE_MCAST,
	PCN_KMSG_TYPE_PROC_SRV_CLONE_REQUEST,
	PCN_KMSG_TYPE_PROC_SRV_CREATE_PROCESS_PAIRING,
	PCN_KMSG_TYPE_PROC_SRV_EXIT_PROCESS,
	PCN_KMSG_TYPE_PROC_SRV_BACK_MIG_REQUEST,
	PCN_KMSG_TYPE_PROC_SRV_VMA_OP,
	PCN_KMSG_TYPE_PROC_SRV_VMA_LOCK,
	PCN_KMSG_TYPE_PROC_SRV_MAPPING_REQUEST,
	PCN_KMSG_TYPE_PROC_SRV_NEW_KERNEL,
	PCN_KMSG_TYPE_PROC_SRV_NEW_KERNEL_ANSWER,
	PCN_KMSG_TYPE_PROC_SRV_MAPPING_RESPONSE,
	PCN_KMSG_TYPE_PROC_SRV_MAPPING_RESPONSE_VOID,
	PCN_KMSG_TYPE_PROC_SRV_INVALID_DATA,
	PCN_KMSG_TYPE_PROC_SRV_ACK_DATA,
	PCN_KMSG_TYPE_PROC_SRV_THREAD_COUNT_REQUEST,
	PCN_KMSG_TYPE_PROC_SRV_THREAD_COUNT_RESPONSE,
	PCN_KMSG_TYPE_PROC_SRV_THREAD_GROUP_EXITED_NOTIFICATION,
	PCN_KMSG_TYPE_PROC_SRV_VMA_ACK,
	PCN_KMSG_TYPE_PROC_SRV_BACK_MIGRATION,
	PCN_KMSG_TYPE_PCN_PERF_START_MESSAGE,
	PCN_KMSG_TYPE_PCN_PERF_END_MESSAGE,
	PCN_KMSG_TYPE_PCN_PERF_CONTEXT_MESSAGE,
	PCN_KMSG_TYPE_PCN_PERF_ENTRY_MESSAGE,
	PCN_KMSG_TYPE_PCN_PERF_END_ACK_MESSAGE,
	PCN_KMSG_TYPE_START_TEST,
	PCN_KMSG_TYPE_REQUEST_TEST,
	PCN_KMSG_TYPE_ANSWER_TEST,
	PCN_KMSG_TYPE_MCAST_CLOSE,
	PCN_KMSG_TYPE_SHMTUN,
		PCN_KMSG_TYPE_REMOTE_PROC_MEMINFO_REQUEST,
		PCN_KMSG_TYPE_REMOTE_PROC_MEMINFO_RESPONSE,
		PCN_KMSG_TYPE_REMOTE_PROC_STAT_REQUEST,
		PCN_KMSG_TYPE_REMOTE_PROC_STAT_RESPONSE,
		PCN_KMSG_TYPE_REMOTE_PID_REQUEST,
		PCN_KMSG_TYPE_REMOTE_PID_RESPONSE,
		PCN_KMSG_TYPE_REMOTE_PID_STAT_REQUEST,
		PCN_KMSG_TYPE_REMOTE_PID_STAT_RESPONSE,
		PCN_KMSG_TYPE_REMOTE_PID_CPUSET_REQUEST,
		PCN_KMSG_TYPE_REMOTE_PID_CPUSET_RESPONSE,
		PCN_KMSG_TYPE_REMOTE_SENDSIG_REQUEST,
		PCN_KMSG_TYPE_REMOTE_SENDSIG_RESPONSE,
		PCN_KMSG_TYPE_REMOTE_SENDSIGPROCMASK_REQUEST,
		PCN_KMSG_TYPE_REMOTE_SENDSIGPROCMASK_RESPONSE,
		PCN_KMSG_TYPE_REMOTE_SENDSIGACTION_REQUEST,
		PCN_KMSG_TYPE_REMOTE_SENDSIGACTION_RESPONSE,
		PCN_KMSG_TYPE_REMOTE_IPC_SEMGET_REQUEST,
		PCN_KMSG_TYPE_REMOTE_IPC_SEMGET_RESPONSE,
		PCN_KMSG_TYPE_REMOTE_IPC_SEMCTL_REQUEST,
		PCN_KMSG_TYPE_REMOTE_IPC_SEMCTL_RESPONSE,
		PCN_KMSG_TYPE_REMOTE_IPC_SHMGET_REQUEST,
		PCN_KMSG_TYPE_REMOTE_IPC_SHMGET_RESPONSE,
		PCN_KMSG_TYPE_REMOTE_IPC_SHMAT_REQUEST,
		PCN_KMSG_TYPE_REMOTE_IPC_SHMAT_RESPONSE,
		PCN_KMSG_TYPE_REMOTE_IPC_FUTEX_WAKE_REQUEST,
		PCN_KMSG_TYPE_REMOTE_IPC_FUTEX_WAKE_RESPONSE,
		PCN_KMSG_TYPE_REMOTE_PFN_REQUEST,
		PCN_KMSG_TYPE_REMOTE_PFN_RESPONSE,
		PCN_KMSG_TYPE_REMOTE_IPC_FUTEX_KEY_REQUEST,
		PCN_KMSG_TYPE_REMOTE_IPC_FUTEX_KEY_RESPONSE,
		PCN_KMSG_TYPE_REMOTE_IPC_FUTEX_TOKEN_REQUEST,
	PCN_KMSG_TYPE_REMOTE_PROC_CPUINFO_RESPONSE,
	PCN_KMSG_TYPE_REMOTE_PROC_CPUINFO_REQUEST,
	PCN_KMSG_TYPE_PROC_SRV_CREATE_THREAD_PULL,
	PCN_KMSG_TERMINATE,
	PCN_KMSG_TYPE_SELFIE_TEST,
	PCN_KMSG_TYPE_MAX
};

/* Enum for message priority. */
enum pcn_kmsg_prio {
	PCN_KMSG_PRIO_HIGH,
	PCN_KMSG_PRIO_NORMAL
};

#define __READY_SIZE 1
#define LG_SEQNUM_SIZE  (8 - __READY_SIZE)

/* Message header */
struct pcn_kmsg_hdr {
	unsigned int from_cpu	:8; // b0

	enum pcn_kmsg_type type	:8; // b1

	enum pcn_kmsg_prio prio	:5; // b2
	unsigned int is_lg_msg  :1;
	unsigned int lg_start   :1;
	unsigned int lg_end     :1;

	unsigned long long_number; // b3 .. b10

	unsigned int lg_seqnum 	:LG_SEQNUM_SIZE; // b11
	unsigned int __ready	:__READY_SIZE;
	unsigned int size;
	unsigned int slot;
	unsigned int conn_no;
}__attribute__((packed));

#define CACHE_LINE_SIZE 64
#define PCN_KMSG_PAYLOAD_SIZE (CACHE_LINE_SIZE - sizeof(struct pcn_kmsg_hdr))

#define MAX_CHUNKS ((1 << LG_SEQNUM_SIZE) -1)
#define PCN_KMSG_LONG_PAYLOAD_SIZE (MAX_CHUNKS*PCN_KMSG_PAYLOAD_SIZE)

/* The actual messages.  The expectation is that developers will create their
   own message structs with the payload replaced with their own fields, and then
   cast them to a struct pcn_kmsg_message.  See the checkin message below for
   an example of how to do this. */

/* Struct for the actual messages.  Note that hdr and payload are flipped
   when this actually goes out, so the receiver can poll on the ready bit
   in the header. */
struct pcn_kmsg_message {
	struct pcn_kmsg_hdr hdr;
	unsigned char payload[PCN_KMSG_PAYLOAD_SIZE];
}__attribute__((packed)) __attribute__((aligned(CACHE_LINE_SIZE)));

/* Struct for sending long messages (>60 bytes payload) */
struct pcn_kmsg_long_message {
	struct pcn_kmsg_hdr hdr;
	unsigned char payload[PCN_KMSG_LONG_PAYLOAD_SIZE];
}__attribute__((packed));

/* TYPES OF MESSAGES */

/* Message struct for guest kernels to check in with each other. */
struct pcn_kmsg_checkin_message {
	struct pcn_kmsg_hdr hdr;
	unsigned long window_phys_addr;
	unsigned char cpu_to_add;
	char pad[51];
}__attribute__((packed)) __attribute__((aligned(CACHE_LINE_SIZE)));

/* FUNCTIONS */

/* Typedef for function pointer to callback functions */
typedef int (*pcn_kmsg_cbftn)(struct pcn_kmsg_message *);

/* Typedef for function pointer to callback functions */
typedef int (*send_cbftn)(unsigned int, struct pcn_kmsg_message *, unsigned int);

/* SETUP */

/* Register a callback function to handle a new message type.  Intended to
   be called when a kernel module is loaded. */
int pcn_kmsg_register_callback(enum pcn_kmsg_type type,
		pcn_kmsg_cbftn callback);

/* Unregister a callback function for a message type.  Intended to
   be called when a kernel module is unloaded. */
int pcn_kmsg_unregister_callback(enum pcn_kmsg_type type);

/* MESSAGING */

/* Send a message to the specified destination CPU. */
int pcn_kmsg_send(unsigned int dest_cpu, struct pcn_kmsg_message *msg);

/* Send a long message to the specified destination CPU. */
int pcn_kmsg_send_long(unsigned int dest_cpu,
		struct pcn_kmsg_long_message *lmsg,
		unsigned int payload_size);

/* Free a received message (called at the end of the callback function) */
inline void pcn_kmsg_free_msg(void *msg);

/* MULTICAST GROUPS */

/* Enum for mcast message type. */
enum pcn_kmsg_mcast_type {
	PCN_KMSG_MCAST_OPEN,
	PCN_KMSG_MCAST_ADD_MEMBERS,
	PCN_KMSG_MCAST_DEL_MEMBERS,
	PCN_KMSG_MCAST_CLOSE,
	PCN_KMSG_MCAST_MAX
};

/* Message struct for guest kernels to check in with each other. */
struct pcn_kmsg_mcast_message {
	struct pcn_kmsg_hdr hdr;
	enum pcn_kmsg_mcast_type type :32;
	pcn_kmsg_mcast_id id;
	unsigned long mask;
	unsigned int num_members;
	unsigned long window_phys_addr;
	char pad[28];
}__attribute__((packed)) __attribute__((aligned(CACHE_LINE_SIZE)));

/* Open a multicast group containing the CPUs specified in the mask. */
int pcn_kmsg_mcast_open(pcn_kmsg_mcast_id *id, unsigned long mask);

/* Add new members to a multicast group. */
int pcn_kmsg_mcast_add_members(pcn_kmsg_mcast_id id, unsigned long mask);

/* Remove existing members from a multicast group. */
int pcn_kmsg_mcast_delete_members(pcn_kmsg_mcast_id id, unsigned long mask);

/* Close a multicast group. */
int pcn_kmsg_mcast_close(pcn_kmsg_mcast_id id);

/* Send a message to the specified multicast group. */
int pcn_kmsg_mcast_send(pcn_kmsg_mcast_id id, struct pcn_kmsg_message *msg);

/* Send a long message to the specified multicast group. */
int pcn_kmsg_mcast_send_long(pcn_kmsg_mcast_id id,
		struct pcn_kmsg_long_message *msg,
		unsigned int payload_size);

int pcn_kmsg_get_node_ids(uint16_t *nodes, int len, uint16_t *self);

#endif /* __LINUX_PCN_KMSG_H */
