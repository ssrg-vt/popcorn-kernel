/*
 * pcn_kmesg.c - Kernel Module for Popcorn Messaging Layer over Socket
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/semaphore.h>
#include <linux/file.h>
#include <linux/ktime.h>


#include <linux/fdtable.h>

#include <linux/net.h>
#include <net/sock.h>
#include <linux/tcp.h>
#include <linux/in.h>
#include <asm/uaccess.h>
#include <linux/socket.h>

#include <linux/delay.h>
#include <linux/time.h>
#include <asm/atomic.h>
#include <linux/completion.h>

#include <linux/cpumask.h>
#include <linux/sched.h>

#include <linux/vmalloc.h>
//#include "genif.h"

/* geting host ip */
#include <linux/netdevice.h>
#include <linux/inetdevice.h>

#include <popcorn/debug.h>
#include <popcorn/pcn_kmsg.h>

/* global */
int my_cpu; // source

#pragma GCC diagnostic ignored "-Wdeclaration-after-statement"
/**
 * Jack
 *  mssg layer multi-version
 *  msg sent to data_sock[conn_no] according to dest_cpu
 */

/////////////////////Jack's testing & example code/////////////////////////////////////
typedef struct {
    struct pcn_kmsg_hdr header; // must followd
    // you define
    int example1;
    int example2;
}__attribute__((packed)) remote_thread_first_test_request_t; // for cache

static void handle_remote_thread_first_test_request(struct pcn_kmsg_message* inc_msg)
{
    remote_thread_first_test_request_t *request =
			(remote_thread_first_test_request_t *)inc_msg;

    MSGPRINTK("<<<<< Jack MSG_LAYER SELF-TESTING: "
			"my_cpu=%d from_cpu=%d example1=%d example2=%d >>>>>\n",
            my_cpu, request->header.from_cpu,
			request->example1, request->example2);

    /* extra examples */
    // sync
    //down_read(&mm_data->kernel_set_sem);

    // new work
    //INIT_WORK( (struct work_struct*)request_work,
    //                   process_count_request);
    //queue_work(exit_wq, (struct work_struct*) request_work);

    return;
    // if you wanna do pong, plz remember you have to have another
    // struct remote_thread_first_test_response_t
}
/////////////////////Jack's testing & example code/////////////////////////////////////

/* Machines info !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!! */
#define MAX_NUM_NODES       2  // total num of machines
#define MAX_NUM_CHANNELS    MAX_NUM_NODES-1 //=MAX

const char *net_dev_names[] = {
	"eth0",		// Testing
	"ib0",		// InfiniBand
	"p7p1",		// Xgene (ARM)
} ;  // testing
const uint32_t ip_table[MAX_NUM_NODES] = {
	IP_TO_UINT32(10, 0, 0, 100),
	IP_TO_UINT32(10, 0, 0, 101),
};
/*
uint32_t ip_table[MAX_NUM_NODES] ={
		(192<<24 | 168<<16 | 69<<8 | 127),      // echo3 ib0
		(192<<24 | 169<<16 | 69<<8 | 128),      // echo4 ib0
		(192<<24 | 168<<16 | 69<<8 | 254) };    // none ib0
*/


/* sock definitions */
#define MAX_NUM_BUF         20
#define MAX_ASYNC_BUFFER  1024 // num of rbuffer for each conn (numb for vmalloc)
#define PORT 1000 //Jack: TODO: NEED TO CHANGE TO A ARRAY FOR MULTIPLE CONNECTIONS

/** This is only for 2 nodes setup**/
/** TODO for multiple connections, we need to know everyone's IP **/
//#define INADDR_SEND (10<<24 | 1<<16 | 1<<8 | 155) // server ip

static int connect_thread(void *arg0); // kernel thread for waiting signal and then deq()
static int accept_handler(void* arg0);
//int sock_kmsg_send_long(unsigned int dest_cpu, struct pcn_kmsg_long_message *lmsg, unsigned int payload_size, int conn_no); // triggered by user, doing enq() and then sending signal
int sock_kmsg_send_long(unsigned int dest_cpu, struct pcn_kmsg_long_message *lmsg, unsigned int payload_size); // triggered by user, doing enq() and then sending signal

//** sock init **//
struct sockaddr_in dest_addr;
struct sockaddr_in serv_addr;
struct socket *sock_data[MAX_NUM_NODES];
struct socket *sock_listen; // server port (same for each node) each one has ONLY a port for listing.

//** Popcorn utility **//
static int ksock_send(struct socket *sock, char *buf, int len); // popcorn utility - wrap popinfo, send through sock
static int ksock_recv(struct socket *sock, char *buf, int len); // popcorn utility - wrap popinfo, send through sock
uint32_t get_host_ip(char **name_ret);

//save number (MAX_ASYNC_BUFFER) of pcn_kmsg_buf
static struct pcn_kmsg_buf *send_buf[MAX_NUM_NODES];
static struct pcn_kmsg_buf *recv_buf[MAX_NUM_NODES];

volatile static int is_connection_done[MAX_NUM_NODES]; //Jack: atomic is more safe / use smp_mb()

struct pcn_kmsg_buf_item {
    struct pcn_kmsg_long_message *msg;
    unsigned int dest_cpu;
    unsigned int payload_size;
};

//Jack
struct pcn_kmsg_buf { // we got 1024 this // kmalloc()ed
    struct pcn_kmsg_buf_item *rbuf; // for recycle // this is allocated by vmalloc others kmalloc
    unsigned long head;
    unsigned long tail;
    spinlock_t enq_buf_mutex;
    spinlock_t deq_buf_mutex;
    struct semaphore q_empty;
    struct semaphore q_full;
    //Jack: put conn_no here? better for understanding the code?

    //- ? -//
    //atomic_t q_is_empty;    // is this a optimization?

    //- pcie legacy -//
    //int is_free;
    //int status;     // is_enqued
    //char* buff;

//TODO: Jack: make socket version _send_wait
//typedef struct _send_wait{ //msg meta //enq/deq target // inherent struct pcn_kmsg_buf
//        struct list_head list;   //// list element
////        struct semaphore _sem;
//        void * msg;
//        int error;
//        int dst_cpu;
//	    pool_buffer_t *assoc_buf; // send_data->assoc_buf = &send_buf[i];
//}send_wait; // <<- link list!!!!!!!!!!!!!!!!!!!!!!!!for enq() & deq()
};

typedef struct _send_thread_data
{
    int conn_no;
    struct pcn_kmsg_buf *buf;
} send_thread_data;

typedef struct _conn_thread_data // replace _recv_data
{
    int conn_no;
    struct pcn_kmsg_buf *buf; //Jack
    //int is_worker;
} conn_thread_data;

typedef struct _exec_thread_data
{
    int conn_no;
    struct pcn_kmsg_buf *buf;
} exec_thread_data;


/* for debug */
// check POPCORN_MSG_LAYER_VERBOSE in <linux/pcn_kmsg.h>
#define MSG_TEST 	0

struct task_struct *handler[MAX_NUM_NODES];          // thread for each connection(slot)
struct task_struct *sender_handler[MAX_NUM_NODES];   // thread for each connection(slot)
struct task_struct *execution_handler[MAX_NUM_NODES];   // thread for each connection(slot)

//* sync - completion *//
/*
 *  wait_for_completion_interruptible() - if recvs a TIF_SIGPENDING signal, kill the task from the queue
 */
struct completion send_q_mutex;        // lock for send queue Jack: attention!!!!!!!!!!!!

struct completion send_completion[MAX_NUM_NODES];
struct completion recv_completion[MAX_NUM_NODES];

static struct semaphore connect_sem[MAX_NUM_NODES];  // sync in the end of init/before while(1) for waiting connection established
static struct semaphore accept_sem[MAX_NUM_NODES];   // sync in the end of init/before while(1) for waiting connection established

static int __init initialize(void);


extern pcn_kmsg_cbftn callbacks[PCN_KMSG_TYPE_MAX];
extern send_cbftn send_callback;

/* for testing
char *msg_names[] = {
	"TEST",
	"TEST_LONG",
	"CHECKIN",
	"MCAST",
	"PROC_SRV_CLONE_REQUEST",
	"PROC_SRV_CREATE_PROCESS_PAIRING",
	"PROC_SRV_EXIT_PROCESS",
	"PROC_SRV_BACK_MIG_REQUEST",
	"PROC_SRV_VMA_OP",
	"PROC_SRV_VMA_LOCK",
	"PROC_SRV_MAPPING_REQUEST",
	"PROC_SRV_NEW_KERNEL",
	"PROC_SRV_NEW_KERNEL_ANSWER",
	"PROC_SRV_MAPPING_RESPONSE",
	"PROC_SRV_MAPPING_RESPONSE_VOID",
	"PROC_SRV_INVALID_DATA",
	"PROC_SRV_ACK_DATA",
	"PROC_SRV_THREAD_COUNT_REQUEST",
	"PROC_SRV_THREAD_COUNT_RESPONSE",
	"PROC_SRV_THREAD_GROUP_EXITED_NOTIFICATION",
	"PROC_SRV_VMA_ACK",
	"PROC_SRV_BACK_MIGRATION",
	"PCN_PERF_START_MESSAGE",
	"PCN_PERF_END_MESSAGE",
	"PCN_PERF_CONTEXT_MESSAGE",
	"PCN_PERF_ENTRY_MESSAGE",
	"PCN_PERF_END_ACK_MESSAGE",
	"START_TEST",
	"REQUEST_TEST",
	"ANSWER_TEST",
	"MCAST_CLOSE",
	"SHMTUN",
	"REMOTE_PROC_MEMINFO_REQUEST",
	"REMOTE_PROC_MEMINFO_RESPONSE",
	"REMOTE_PROC_STAT_REQUEST",
	"REMOTE_PROC_STAT_RESPONSE",
	"REMOTE_PID_REQUEST",
	"REMOTE_PID_RESPONSE",
	"REMOTE_PID_STAT_REQUEST",
	"REMOTE_PID_STAT_RESPONSE",
	"REMOTE_PID_CPUSET_REQUEST",
	"REMOTE_PID_CPUSET_RESPONSE",
	"REMOTE_SENDSIG_REQUEST",
	"REMOTE_SENDSIG_RESPONSE",
	"REMOTE_SENDSIGPROCMASK_REQUEST",
	"REMOTE_SENDSIGPROCMASK_RESPONSE",
	"REMOTE_SENDSIGACTION_REQUEST",
	"REMOTE_SENDSIGACTION_RESPONSE",
	"REMOTE_IPC_SEMGET_REQUEST",
	"REMOTE_IPC_SEMGET_RESPONSE",
	"REMOTE_IPC_SEMCTL_REQUEST",
	"REMOTE_IPC_SEMCTL_RESPONSE",
	"REMOTE_IPC_SHMGET_REQUEST",
	"REMOTE_IPC_SHMGET_RESPONSE",
	"REMOTE_IPC_SHMAT_REQUEST",
	"REMOTE_IPC_SHMAT_RESPONSE",
	"REMOTE_IPC_FUTEX_WAKE_REQUEST",
	"REMOTE_IPC_FUTEX_WAKE_RESPONSE",
	"REMOTE_PFN_REQUEST",
	"REMOTE_PFN_RESPONSE",
	"REMOTE_IPC_FUTEX_KEY_REQUEST",
	"REMOTE_IPC_FUTEX_KEY_RESPONSE",
	"REMOTE_IPC_FUTEX_TOKEN_REQUEST",
	"REMOTE_PROC_CPUINFO_RESPONSE",
	"REMOTE_PROC_CPUINFO_REQUEST",
	"PROC_SRV_CREATE_THREAD_PULL",
	"PCN_KMSG_TERMINATE",
	"SELFIE_TEST",
	"FILE_MIGRATE_REQUEST",
	"FILE_OPEN_REQUEST",
	"FILE_OPEN_REPLY",
	"FILE_STATUS_REQUEST",
	"FILE_STATUS_REPLY",
	"FILE_OFFSET_REQUEST",
	"FILE_OFFSET_REPLY",
	"FILE_CLOSE_NOTIFICATION",
	"FILE_OFFSET_UPDATE",
	"FILE_OFFSET_CONFIRM",
	"FILE_LSEEK_NOTIFICATION",
	"SCHED_PERIODIC"
};
*/

void *pcn_kmsg_alloc_msg(size_t size)
{
    struct pcn_kmsg_message *msg = vmalloc(size);
	msg->header.size = size;
    return msg;
}

static int ksock_send(struct socket *sock, char *buf, int len)
{
    struct msghdr msg;
    struct iovec iov;
    int size;
    mm_segment_t oldfs;

    iov.iov_base = buf;
    iov.iov_len = len;

    msg.msg_control = NULL;
    msg.msg_controllen = 0;
    msg.msg_flags = 0;
    //msg.msg_iov = &iov;       // 3.12
    msg.msg_iter.iov = &iov;    // 4.4
    //msg.msg_iovlen = 1;       // 3.12
    //msg.msg_iter.count    = 1;   // 4.4 Jack was wrong
    msg.msg_iter.count      = len; // 4.4 Sang-Hoon
    msg.msg_iter.nr_segs    = 1;   // 4.4 Sang-Hoon
    msg.msg_iter.iov_offset = 0;   // 4.4 Sang-Hoon

    msg.msg_name = NULL;    //Jack msg_names[temp->header.type]
    msg.msg_namelen = 0;

    oldfs = get_fs();
    set_fs(KERNEL_DS);
    // size = sock_sendmsg(sock, &msg, len); // 3.12
    size = sock_sendmsg(sock, &msg); // 4.4
    set_fs(oldfs);

    return size;
}

static int ksock_recv(struct socket *sock, char *buf, int len)
{
    struct msghdr msg;
    struct iovec iov;
    int size;
    mm_segment_t oldfs;

    iov.iov_base = buf;
    iov.iov_len = len;

    msg.msg_flags   = 0;
    msg.msg_control = NULL;
    msg.msg_controllen = 0;
    //msg.msg_iov   = &iov;     // 3.12
    msg.msg_iter.iov = &iov;    // 4.4
    //msg.msg_iovlen = 1;       // 3.12
    //msg.msg_iter.count    = 1;   // 4.4 Jack was wrong
    msg.msg_iter.count      = len; // 4.4 Sang-Hoon
    msg.msg_iter.nr_segs    = 1;   // 4.4 Sang-Hoon
    msg.msg_iter.iov_offset = 0;   // 4.4 Sang-Hoon

    msg.msg_name = NULL;    //Jack: msg_names[temp->hdr.type])
    msg.msg_namelen = 0;

    oldfs = get_fs();
    set_fs(KERNEL_DS);
    size = sock_recvmsg(sock, &msg, len, msg.msg_flags);
    set_fs(oldfs);

    return size;
}

/* now is polling, not using this function right now
 * will be replaced with enq_send()
 */
static int enq_send(struct pcn_kmsg_buf *buf, struct pcn_kmsg_message *msg, unsigned int dest_cpu, unsigned int payload_size, int conn_no)
{
    int err;
	unsigned long head;
    MSGDPRINTK("Jackmsglayer: enq_send-1 not used right now\n");

    err = down_interruptible(&(buf->q_full));
    if (err!=0) // testing - 0:correct others:wrong
        return err;
    spin_lock(&(buf->enq_buf_mutex));
	head = buf->head;
    //unsigned long tail = ACCESS_ONCE(buf->tail); // unused

    buf->rbuf[head].msg = (struct pcn_kmsg_buf_item*) msg; // testing
    buf->rbuf[head].dest_cpu = dest_cpu;
    buf->rbuf[head].payload_size = payload_size;
    smp_wmb();
    buf->head = (head + 1) & (MAX_ASYNC_BUFFER - 1);
    up(&(buf->q_empty));    //send q_empty++
    spin_unlock(&(buf->enq_buf_mutex));

    //complete(&send_completion[dest_cpu]);  // JACK: send to dest sock
    return 0;
}

static int deq_send(struct pcn_kmsg_buf * buf, int conn_no)
{
    int err;
    struct pcn_kmsg_buf_item msg;
    if (is_connection_done[conn_no] == 0) { // if still waiting for connecting
        msleep(50);
        return -1;
    }

    MSGDPRINTK("Jackmsglayer: deq_send-1 conn_no=%d\n", conn_no);
    wait_for_completion(&send_completion[conn_no]); // Jack: make it with using completion
    err = down_interruptible(&(buf->q_empty));      // Jack: TODO: check whethere it's needed since here can be concurrently executed
    if (err!=0) // testing - 0:correct others:wrong
        return err;

    spin_lock(&(buf->deq_buf_mutex));               // Jack: TODO: check whethere it's needed since the same reason
    //unsigned long head = ACCESS_ONCE(buf->head);  // unused
    unsigned long tail = buf->tail;

    smp_read_barrier_depends();
    msg = buf->rbuf[tail];
    smp_mb();
    buf->tail = (tail + 1) & (MAX_ASYNC_BUFFER - 1);
    up(&(buf->q_full));     //send q_empty++
    spin_unlock(&(buf->deq_buf_mutex));

    //TODO: Jack: NOT YET DONE!!!!!!!! do real send here!!!!!!!!!!!!!!!!!!
    //_pcn_do_send(msg.dest_cpu, msg.msg, msg.payload_size, conn_no);
    //sock_kmsg_send_long(msg.dest_cpu, msg.msg, msg.payload_size, conn_no); //JACK: this is wen's version in progress,  SO CHECK
    // wrogng this is for user

    //pcn_kmsg_free_msg(msg.msg); // must be freed by programmer
    return 0;
}

static int enq_recv(struct pcn_kmsg_buf *buf, struct pcn_kmsg_message *msg, int conn_no)
{
    int err;
    MSGDPRINTK("Jackmsglayer: enq_recv-1 conn_no=%d\n", conn_no);

    err = down_interruptible(&(buf->q_full));
    if (err!=0) // testing - 0:correct others:wrong
        return err;

    spin_lock(&(buf->enq_buf_mutex));
    unsigned long head = buf->head;
    //unsigned long tail = ACCESS_ONCE(buf->tail); // unused

    buf->rbuf[head].msg = msg;
    smp_wmb();
    buf->head = (head + 1) & (MAX_ASYNC_BUFFER - 1);
    up(&(buf->q_empty));    //recv q_empty++
    spin_unlock(&(buf->enq_buf_mutex));

    complete(&recv_completion[conn_no]); //Jack
    return 0;
}

static int deq_recv(struct pcn_kmsg_buf *buf, int conn_no)
{
    int err;
    struct pcn_kmsg_buf_item msg;
    pcn_kmsg_cbftn ftn; // function pointer - typedef int (*pcn_kmsg_cbftn)(struct pcn_kmsg_message *);

    if (is_connection_done[conn_no] == 0) { // if still waiting for connecting
        msleep(50);
        return;
    }

    MSGDPRINTK("Jackmsglayer: deq_recv-1 conn_no=%d\n", conn_no);
    // TODO: replace spinlock with wait_for_complete. / wait_for_complete_interruptable() // really?
    wait_for_completion(&recv_completion[conn_no]); //TODO: the same as above
    MSGDPRINTK("Jackmsglayer: deq_recv-2\n");

    err = down_interruptible(&(buf->q_empty));
    if (err!=0) // testing - 0:correct others:wrong
        return err;
    MSGDPRINTK("Jackmsglayer: deq_recv-3\n");
    spin_lock(&(buf->deq_buf_mutex));
    //unsigned long head = ACCESS_ONCE(buf->head); // unused
    unsigned long tail = buf->tail;

    smp_read_barrier_depends();
    msg = buf->rbuf[tail];
    smp_mb();
    buf->tail = (tail + 1) & (MAX_ASYNC_BUFFER - 1);
    up(&(buf->q_full));     //recv q_empty++
    spin_unlock(&(buf->deq_buf_mutex));

    if (msg.msg->header.type < 0 || msg.msg->header.type >= PCN_KMSG_TYPE_MAX){
        MSGDPRINTK(KERN_INFO "Received invalid message type %d\n",
				msg.msg->header.type);
        pcn_kmsg_free_msg(msg.msg);
    }else{
        ftn = callbacks[msg.msg->header.type];
        if (ftn != NULL){
            ftn(msg.msg); // Jack: invoke callback with input arguments
        }else{
            MSGDPRINTK(KERN_INFO "Recieved message type %d size %d "
					"has no registered callback!\n",
					msg.msg->header.type,msg.msg->header.size);
            pcn_kmsg_free_msg(msg.msg);
        }
    }
    return 0;
}

//Jack utility -
uint32_t get_host_ip(char **name_ret)
{
    // way1.
    /*
    struct net_device *dev = __dev_get_by_name("eth0");
    dev->dev_addr; // is the MAC address
    dev->stats.rx_dropped; // RX dropped packets. (stats has more statistics)

    struct in_device *in_dev = rcu_dereference(dev->ip_ptr);
    // in_dev has a list of IP addresses (because an interface can have multiple)
    struct in_ifaddr *ifap;
    for (ifap = in_dev->ifa_list; ifap != NULL;
        ifap = ifa1->ifa_next) {
        ifap->ifa_address; // is the IPv4 address
    }
    */
    // way2.
    struct net_device *device;
    struct in_device *in_dev;
    struct in_ifaddr *if_info;
	int i;

    __u8 *addr;

    //sprintf(net_dev_name,"eth%d",ethernet_id);  // =strcat()
	for (i = 0; i < sizeof(net_dev_names) / sizeof(*net_dev_names); i++) {
		const char *name = net_dev_names[i];
		device = __dev_get_by_name(&init_net, name); // namespace=normale
		if (device) {
			*name_ret = name;
			in_dev = (struct in_device *)device->ip_ptr;
			if_info = in_dev->ifa_list;
			addr = (char *)&if_info->ifa_local;
			MSGDPRINTK(KERN_WARNING "Device %s IP: %u.%u.%u.%u\n",
							name,
							(__u32)addr[0],
							(__u32)addr[1],
							(__u32)addr[2],
							(__u32)addr[3]);
			return IP_TO_UINT32(addr[0], addr[1], addr[2], addr[3]);
		}
	}
	MSGPRINTK(KERN_ERR "Jackmsglayer: ERROR - cannot find host ip\n");
	return -1;
}


// Initialize callback table to null, set up control and data channels
int __init initialize()
{
    int i, err;
	char *name;
    //TODO: check how to assign a priority to these threads! make msg_layer faster (higher prio)
	//struct sched_param param = {.sched_priority = 10};
	MSGPRINTK("--- Popcorn messaging layer init starts ---\n");

    for (i=0; i<MAX_NUM_NODES; i++) {
        if (get_host_ip(&name) == ip_table[i])  {
            my_cpu=i;
            MSGPRINTK("Device \"%s\" my_cpu=%d on machine IP %u.%u.%u.%u\n",
                                                name, my_cpu,
                                                (ip_table[i]>>24)&0x000000ff,
                                                (ip_table[i]>>16)&0x000000ff,
                                                (ip_table[i]>> 8)&0x000000ff,
                                                (ip_table[i]>> 0)&0x000000ff);
            break;
        }
    }
    if (my_cpu==-99)
        MSGPRINTK(KERN_ERR "Jackmsglayer: ERROR ERROR ERROR\n");

    smp_mb(); // Jack: since my_cpu is extern (global)
	MSGPRINTK("----------------------------------------------------------\n");
	MSGPRINTK("------ updating to my_cpu=%d wait for a moment -----------\n", my_cpu);
	MSGPRINTK("----------------------------------------------------------\n");
	MSGDPRINTK("MSG_LAYER: Initialization my_cpu=%d\n", my_cpu);

	for (i = 0; i<MAX_NUM_NODES; i++) {
		init_completion(&send_completion[i]); // Jack:
		init_completion(&recv_completion[i]); // Jack:

        sema_init(&connect_sem[i],0);  // for waiting connection established
        sema_init(&accept_sem[i],0);   // for waiting connection established

        // Jack: connection lable buf
        is_connection_done[i]=PCN_CONN_WATING; // connection status buf (each slot refers to each port)
	}

	/* Initilaize the sock */
    /*
     *  Each node has a connection table like tihs:
     * -----------------------------------------------------------------------------
     * | connect | connect | (many)... | my_cpu(one) | accept | accept | (many)... |
     * -----------------------------------------------------------------------------
     * my_cpu:  no need to talk to itself
     * connect: connecting to existing nodes
     * accept:  waiting for the connection requests from later nodes
     */
    // 0. init sock listening port //(can be a func.) // connect , [my_cpu], accept
    sock_listen = (struct socket*)kmalloc(sizeof(struct socket),GFP_KERNEL);
    err = sock_create(PF_INET, SOCK_STREAM, IPPROTO_TCP, &sock_listen);
    if (err < 0) {
        MSGDPRINTK("Failed to create socket..!! Messaging layer init failed with err %d\n", err);
        return err;
    }

    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = INADDR_ANY;
    serv_addr.sin_port = htons(PORT); // server port (same for each node) each one has ONLY a port for listing.

    err = sock_listen->ops->bind(sock_listen, (struct sockaddr *)&serv_addr,
    sizeof(serv_addr));
    if (err < 0) {
        MSGDPRINTK("Failed to bind connection..!! Messaging layer init failed\n");
        sock_release(sock_listen);
        sock_listen = NULL;
        is_connection_done[my_cpu] = 0;
        return err;
    }

    err = sock_listen->ops->listen(sock_listen, 1);
    if (err < 0) {
        MSGDPRINTK("Failed to listen on connection..!! Messaging layer init failed\n");
        sock_release(sock_listen);
        sock_listen = NULL;
        is_connection_done[my_cpu] = 0;
        return err;
    }
    // Jack: should I set it as 1 later?
    is_connection_done[my_cpu] = 1; // Jack: atomic is more safe? // Take node 1 for example. connect0 [my_cpu(1)] accept2
    smp_mb(); // Jack: MUST HAVE. actually I guess mb() is better than atomic since just one shot
    MSGDPRINTK(" server successfully listening on socket port=%d\n", PORT);

    // 1. init sock indo for conn thread (recv thread) and start it
    for (i=0; i<MAX_NUM_NODES; i++) { // connect , my_cpu, [accept]
        if (i==my_cpu) continue;
        conn_thread_data * conn_data = (conn_thread_data*) kmalloc(sizeof(conn_thread_data),GFP_KERNEL);
        BUG_ON(!conn_data);

        conn_data->conn_no = i;     // Take node 1 for example. connect0 [1] accept2
        // The circular buffer for received messages
        conn_data->buf = (struct pcn_kmsg_buf *) kmalloc(sizeof(struct pcn_kmsg_buf), GFP_KERNEL);
        BUG_ON(!(conn_data->buf));
        conn_data->buf->rbuf = (struct pcn_kmsg_buf_item *) vmalloc(sizeof(struct pcn_kmsg_buf_item) * MAX_ASYNC_BUFFER);

        conn_data->buf->head = 0;
        conn_data->buf->tail = 0;

        //- legay -//
        //conn_data->buf->is_free = 1;
        //conn_data->buf->status = 0;

        sema_init(&(conn_data->buf->q_empty), 0);
        sema_init(&(conn_data->buf->q_full), MAX_ASYNC_BUFFER);
        recv_buf[i] = conn_data->buf;   // save this pointer to a global arry!
        spin_lock_init(&(conn_data->buf->enq_buf_mutex));
        spin_lock_init(&(conn_data->buf->deq_buf_mutex));

        smp_wmb();

        sender_handler[i] = kthread_run(accept_handler, conn_data, "pcn_connd"); //each thread for each conn // which thread runs msg
        if (sender_handler[i] < 0){
            MSGDPRINTK(KERN_INFO "kthread_run failed! Messaging Layer not initialized\n");
            return (long long int)sender_handler[i];
        }

        //sched_setscheduler(handler[i], SCHED_FIFO, &param);
		//set_cpus_allowed_ptr(handler[i], cpumask_of(i));
    }

    // 2. init sock inf ofor send data thread (send thread) and start it
    //for (i = 0; i < my_cpu; i++) { // [connect] , my_cpu, accept
    for (i=0; i<MAX_NUM_NODES; i++) { // [connect] , my_cpu, accept
        if (i==my_cpu) continue;
        send_thread_data * send_data = (send_thread_data*) kmalloc(sizeof(send_thread_data),GFP_KERNEL);
        BUG_ON(!send_data);

        send_data->conn_no = i;     // Take node 1 for example. connect0 [1] accept2
        send_data->buf = (struct pcn_kmsg_buf *) kmalloc(sizeof(struct pcn_kmsg_buf), GFP_KERNEL);
        BUG_ON(!(send_data->buf));
        send_data->buf->rbuf = (struct pcn_kmsg_buf_item *)vmalloc(sizeof(struct pcn_kmsg_buf_item) * MAX_ASYNC_BUFFER);

        send_data->buf->head = 0;
        send_data->buf->tail = 0;

        //- legay -//
        //send_data->buf->is_free = 1;
        //send_data->buf->status = 0;

        sema_init(&(send_data->buf->q_empty), 0);
        sema_init(&(send_data->buf->q_full), MAX_ASYNC_BUFFER);
        send_buf[i] = send_data->buf;   // save this pointer to a global arry!
        spin_lock_init(&(send_data->buf->enq_buf_mutex));
        spin_lock_init(&(send_data->buf->deq_buf_mutex));

        smp_wmb();

        handler[i] = kthread_run(connect_thread, send_data, "pcn_send_parallel"); //each thread for each conn // which thread runs msg
        if (handler[i] < 0){
            MSGDPRINTK(KERN_INFO "kthread_run failed! Messaging Layer not initialized\n");
            return (long long int)handler[i];
        }

		//sched_setscheduler(sender_handler[i], SCHED_FIFO, &param);
		//set_cpus_allowed_ptr(sender_handler[i], cpumask_of(i%NR_CPUS));
    }

    // Jack: wait for connection done;
	// Jack: multi version will be a problem. Jack: but deq() should check it.
    // Jack: BUTTTTT send_callback are still null pointers so far. We have to wait here!
    for (i=0; i<MAX_NUM_NODES; i++) {
        while (is_connection_done[i]==PCN_CONN_WATING) { //Jack: atomic is more safe
                MSGDPRINTK("waiting for is_connection_done[%d]\n", i);
                msleep(1000);
        }
    }

	send_callback = (send_cbftn) sock_kmsg_send_long;
	smp_mb();
    msleep(1000);
    MSGDPRINTK("Jackmsglayer: msg_layer first broadcasts for popcorn info soulde be launched after all connections are well prepared\n");
    MSGPRINTK("--- Popcorn messaging layer is up ---\n");

	/* Make init popcorn call */
	//_init_RemoteCPUMask(); // msg boradcast //Jack: deal w/ it later

    /* Jack's simple testing if messaging layer healthy & as an example*/ //Jack TODO: move all to another place
    // register callback. also define in <linux/pcn_kmsg.h>
    pcn_kmsg_register_callback(PCN_KMSG_TYPE_TEST,  // ping -
                                handle_remote_thread_first_test_request);
    //pcn_kmsg_register_callback(PCN_KMSG_TYPE_TEST, // pong - usually a pair but just simply test here
    //                            handle_remote_thread_first_test__response);

    msleep(3000); // make sure everyone's registered
    // compose msg - define -> alloc -> essential msg header info
    remote_thread_first_test_request_t* request; // youTODO: make your own struct

    request = (remote_thread_first_test_request_t*) kmalloc(sizeof(remote_thread_first_test_request_t), GFP_ATOMIC);
    if (request==NULL)
        return -1;

    request->header.type = PCN_KMSG_TYPE_TEST;
    request->header.prio = PCN_KMSG_PRIO_NORMAL;
    //request->tgroup_home_cpu = tgroup_home_cpu;
    //request->tgroup_home_id = tgroup_home_id;

    request->example1 = 1;
    request->example2 = 2;

    // send msg - broadcast // Jack TODO: compared with list_for_each_safe, which one is faster
    for (i=0; i<MAX_NUM_NODES; i++){
        if (my_cpu==i)
            continue;
    }
	MSGPRINTK("Jack's testing DONE ! muli-node version msg_layer is healthy !!!!\n");
    msleep(5000);
	MSGDPRINTK(" Value of send ptr = %p\n", send_callback);

	MSGDPRINTK(KERN_INFO "Popcorn Messaging Layer Initialized\n");
	return 0;
}

// For testing
int pcn_kmsg_register_callback(enum pcn_kmsg_type type, pcn_kmsg_cbftn callback)
{
    if (type >= PCN_KMSG_TYPE_MAX)
        return -ENODEV; /* invalid type */

    MSGDPRINTK("%s: registering %d\n",__func__, type);
    callbacks[type] = callback;
    return 0;
}

//Jack: TODO:  NEED TO BE SWITCHED TO BE SOCK
int connect_thread(void* arg0) // for a conn_no
{
    int err = 0;
    //int val = 1; // unused
    send_thread_data *thread_data = (send_thread_data *) arg0;
    int conn_no = thread_data->conn_no;
	MSGDPRINTK("Jackmsglayer: %s(): connect_thread() on conn_no=%d\n", __func__, conn_no);

    //* sock connection init *//
    if ( conn_no < my_cpu ) {
        msleep(10000);  // for prevent both driver executed at the same time (it means the script can execute at the same time)
        err = sock_create(PF_INET, SOCK_STREAM,
            IPPROTO_TCP, &(sock_data[conn_no]));
        if (err < 0) {
            MSGDPRINTK("Failed to create socket..!! Messaging layer init failed with err %d\n", err);
            return err;
        }
        dest_addr.sin_family = AF_INET;
        dest_addr.sin_port = htons(PORT);       // server port (same for each node) each one has ONLY a port for listing.
        dest_addr.sin_addr.s_addr = htonl(ip_table[conn_no]); // target ip (diff)
        MSGDPRINTK("my conn_no=%d connecting to port %d on machine %u.%u.%u.%u\n", conn_no, PORT,
                                                    (ip_table[conn_no]>>24)&0x000000ff,
                                                    (ip_table[conn_no]>>16)&0x000000ff,
                                                    (ip_table[conn_no]>> 8)&0x000000ff,
                                                    (ip_table[conn_no]>> 0)&0x000000ff);
        do {
            err = sock_data[conn_no]->ops->connect(sock_data[conn_no],(struct sockaddr *) &dest_addr,
            sizeof(dest_addr), 0);
            if (err < 0) {
                MSGDPRINTK("Failed to connect to socket..!! Messaging layer init failed with err %d\n", err);
            }
            msleep(100);
        }
        while(err<0);
        up(&connect_sem[conn_no]);
        // For slave, it's connected when connection is established
    } else if ( conn_no > my_cpu ) {
        MSGPRINTK("Jackmsglayer: %s(): my_cpu=%d waiting... for conn_no=%d done on accept_handler()\n", __func__, my_cpu, conn_no);
        err = down_interruptible(&accept_sem[conn_no]);
        if (err!=0) // testing - 0:correct others:wrong
            return err;
        // For master, it's connected when a connection is accepted
    } else if ( conn_no == my_cpu ) {
        MSGPRINTK("Jackmsglayer: %s(): accept() skip myself\n", __func__);
    }
    // sock connection init done
	//up(&send_connDone[channel_num]);
	//MSGDPRINTK("%s: INFO: Connection in connect_thread() succeeds Done...PCN_SEND Thread\n", __func__);

    is_connection_done[conn_no] = 1; //Jack: atomic is more safe
    smp_mb(); // Jack: MUST HAVE. actually I guess mb() is better than atomic since just one shot
    MSGPRINTK("%s(): Connection Established (Done)...PCN_SEND Thread my_cpu=%d conn_no=%d (GOOD)\n", __func__, my_cpu, conn_no);

    for (;;) {
        err = deq_send(send_buf[conn_no], conn_no); // global array
    }

    return 0;
}

int executer_thread(void* arg0)
{
    int err;
    exec_thread_data *data = (exec_thread_data *) arg0;
    int conn_no = data->conn_no;

    for (;;) {
        // TODO: change to
        err = deq_recv(recv_buf[conn_no], conn_no);
    }
}

int accept_handler(void* arg0)
{
    /** after sock_create ->  bind -> listern, now waiting for new connections **/
    // Jack: from sock
    int err;
    struct pcn_kmsg_message *data;
    conn_thread_data *conn_data = (conn_thread_data*) arg0; // a threads handler data

    int conn_no = conn_data->conn_no;

    msleep(100);
    if ( conn_no > my_cpu ) {
        err = sock_create(PF_INET, SOCK_STREAM, IPPROTO_TCP, &sock_data[conn_no]);
        if (err < 0) {
            MSGDPRINTK("Failed to create socket..!! Messaging layer init failed with err %d\n", err);
            goto end;
        }
        MSGPRINTK("Jackmsglayer: %s(): accept()ing... conn_no=%d\n", __func__, conn_no);
        err = sock_listen->ops->accept(sock_listen, sock_data[conn_no], 0); //IMPORTANT!!! and cannot be O_NONBLOCK
        if (err < 0) {
            MSGDPRINTK("Failed to accept connection..!! Messaging layer init failed\n");
            goto exit;
        }
        MSGPRINTK("Jackmsglayer: %s(): accept() conn_no=%d DONE\n", __func__, conn_no);
        up(&accept_sem[conn_no]);
        // For master, it's able to receive messages when a connection is accepted
    } else if (conn_no < my_cpu) {
        MSGPRINTK("Jackmsglayer: %s(): my_cpu=%d just waiting... for conn_no=%d done on connect_thread()\n", __func__, my_cpu, conn_no);
        err = down_interruptible(&connect_sem[conn_no]);
        if (err!=0) // testing - 0:correct others:wrong
            return err;
        // For slave, it's able to receive messages when connection is established
    }
    else if ( conn_no==my_cpu ) {
        MSGPRINTK("Jackmsglayer: %s(): connection() skip myself\n", __func__);
    }
    // kfree(conn_data); // since conn_data is a global pointers arry, don't release
    MSGPRINTK("%s(): my_cpu=%d conn_no=%d ESTABLISHED (GOOD)\n", __func__, my_cpu, conn_no);
    exec_thread_data *exec_data = kmalloc(sizeof(*exec_data), GFP_KERNEL);
    exec_data->conn_no = conn_no;
    execution_handler[conn_no] = kthread_run(executer_thread, exec_data, "pcnscif_execD_pp"); // deq_recv!! really does the callback handler
    MSGPRINTK("%s(): execution damon ESTABLISHED my_cpu=%d conn_no=%d (GOOD)\n", __func__, my_cpu, conn_no);

    //* doese the polling. copy data from sock to kernel *//
    int len;
    int ret;
    size_t offset;
    struct pcn_kmsg_message *msg;
    data = (struct pcn_kmsg_message*) pcn_kmsg_alloc_msg(2 * PAGE_SIZE); //Jack: TODO: check why 2 ??
    if (data == NULL) {
        MSGDPRINTK("Unable to alloc a message\n");
    }

    is_connection_done[conn_no] = 1; //Jack: atomic is more safe
    smp_mb(); // Jack: MUST HAVE. actually I guess mb() is better than atomic since just one shot

    while(1) {
        //if (kthread_should_stop()) { //Jack: I think no need
        //    goto exit;    // check has anyone call thread_stop
        //}

        /* TODO: Jack IMPORTANT for performance !!!!!!!!!!!!!!!!!
         * here we can use complete:
         * the following copy from sock to kernel part uses -> wait_for_completion(&jack_deq_comeplete);  // if receive sth
         * the callback func execution part uses -> complete() or complete_all()
        */

        offset = sizeof(struct pcn_kmsg_hdr);
        ret = ksock_recv(sock_data[conn_no], (char *) data, offset); // blocking here
        MSGDPRINTK ("recv %d in %d (total including hdr)\n", ret, data->header.size);
        len = data->header.size - sizeof(struct pcn_kmsg_hdr);
        msg = pcn_kmsg_alloc_msg(data->header.size);
        memcpy(msg, data, sizeof(*msg));
        for (; len > 0;) {
            ret = ksock_recv(sock_data[conn_no], ((char *) msg) + offset, len);
			printk("recv %d offset %d remain %d\n", ret, offset, len);
            if (ret == -1)
                continue;
            offset += ret;
            len -= ret;
        }

        if (data->header.size < sizeof(struct pcn_kmsg_hdr)) {
            MSGDPRINTK("Corrupt message\n");
            pcn_kmsg_free_msg(msg);
            continue;
        }
        err = enq_recv(recv_buf[conn_no], msg, conn_no);

        /////////////////////////////////periodic debug
#if MSG_TEST
        MSGDPRINTK("is_done_array\t");
        for (i=0; i<MAX_NUM_NODES ;i++)
            MSGDPRINTK("%d\t", is_connection_done[i]);
        MSGDPRINTK("\n");
#endif
    }

    /** all accect() are done **/
exit:
    sock_release(sock_data[conn_no]);
    sock_data[conn_no] = NULL;
end:
    sock_release(sock_listen);
    sock_listen = NULL;
    is_connection_done[conn_no] = 0;
    return err;
}


/***********************************************
 * Jack: refering to from wen's __pcn_do_send()
 * This is a users callback function
 ***********************************************/
//int sock_kmsg_send_long(unsigned int dest_cpu, struct pcn_kmsg_long_message *lmsg, unsigned int payload_size, int conn_no) { // called by user or kernel
int sock_kmsg_send_long(unsigned int dest_cpu, struct pcn_kmsg_long_message *lmsg, unsigned int payload_size) { // called by user or kernel
    volatile int left;
    int size = 0;

    lmsg->header.size = payload_size;
    lmsg->header.from_cpu = my_cpu;

    // Jack: multi-msg_layer - Send msg to itself
    if (dest_cpu==my_cpu) {
        // copy msg data since programmer will free two buffers (send & recv_handler) !!
        struct pcn_kmsg_long_message *msg = pcn_kmsg_alloc_msg(payload_size);
        BUG_ON(!msg);
        memcpy(msg, lmsg, payload_size);

        if (msg->header.type < 0 || msg->header.type >= PCN_KMSG_TYPE_MAX){
            MSGDPRINTK(KERN_INFO "Received invalid message type %d\n", msg->header.type);
            pcn_kmsg_free_msg(msg);
        }else{
            pcn_kmsg_cbftn ftn; // function pointer - typedef int (*pcn_kmsg_cbftn)(struct pcn_kmsg_message *);
            ftn = callbacks[msg->header.type];
            if (ftn != NULL){
                ftn(msg);       // Jack: invoke callback with input arguments
            } else {
                MSGDPRINTK(KERN_INFO "Recieved message type %d size %d has no registered callback!\n",
                                                            msg->header.type, msg->header.size);

                pcn_kmsg_free_msg(msg);
            }
        }
        return 0;
    }

    // Send to others - two way to do. 1. directely send() 2 enq()
    //1.
    left = lmsg->header.size;
    char *p = (char *) lmsg; // p used to move forward
    /////////////////////////////////////////////////////////////////////////////////////////////////////////
    // TODO: //mutex for sync problem // the following example code almost works but NEED TO TEST!!!!!
    // TODO: //mutex for sync problem // DO THE SAME FOR RECV
    // TODO: //mutext_t mutex_sock_data[MAX_NUM_NODES];
    // TODO: static DEFINE_MUTEX(mutex_sock_data[MAX]);
    // TODO: mutext_lock(&mutex_sock_data[dest_cpu]);
    while (left > 0) {
        MSGDPRINTK("%s: my_cpu=%d dest_cpu=%d conn_no=%d\n", __func__, my_cpu, dest_cpu, dest_cpu);
        size = ksock_send(sock_data[dest_cpu], p, lmsg->header.size);
        if (size < 0) {
            msleep(10);
            continue;
        }
        p += size; // p used to move forward // send...| hdr+data | data| data | ... |
        left -= size;
        MSGDPRINTK ("sent %d in %d (total including hdr), left=%d\n", left-size, lmsg->header.size, left);
    }
    // TODO: mutext_unlock(&mutex_sock_data[dest_cpu]);
    MSGDPRINTK("Jackmsglayer: 1 msg snet through dest_cpu=%d\n", dest_cpu);
    /////////////////////////////////////////////////////////////////////////////////////////////////////////
    // TODO: 2. enq() rather than ksock_send
    //enq_send(struct pcn_kmsg_buf * buf, struct pcn_kmsg_message *msg, dest_cpu, payload_size, conn_no);
    //send_data->assoc_buf->status = 1; // pcie
    /////////////////////////////////////////////////////////////////////////////////////////////////////////
    return 0;
}

static void __exit unload(void)
{
	int i;
    MSGPRINTK("Stopping kernel threads\n");
    /** TODO: at least a NULL a pointer below this line **/
	/* To move out of recv_queue */
	for (i = 0; i<MAX_NUM_NODES; i++) {
		complete_all(&send_completion[i]);
		complete_all(&recv_completion[i]);

        //sema_destroy(&connect_sem[i]); // api not found
        //sema_destroy(&accept_sem[i]); // api not found
	}
	msleep(100);

    // these five are local variable
    //kfree(send_data->buf);
    //kfree(conn_data->buf);
    //kfree(send_data);
    //kfree(conn_data);
    //kfree(exec_data);

	/* release */
    MSGPRINTK("Release threadss\n");
	for (i = 0; i<MAX_NUM_NODES; i++) {
        if (handler[i]!=NULL)
            kthread_stop(handler[i]);
        if (sender_handler[i]!=NULL)
            kthread_stop(sender_handler[i]);
        if (execution_handler[i]!=NULL)
            kthread_stop(execution_handler[i]);
        //Jack: TODO: sock release buffer, check(according to) the init
    }

    //TODO: release socket
    MSGPRINTK("Release sockets\n");
    sock_release(sock_listen);
	for (i = 0; i<MAX_NUM_NODES; i++) {
        if (sock_data[i]!=NULL) // TODO: see if we need this
            sock_release(sock_data[i]);
    }
    //sock_release(sock_listen);

    MSGPRINTK("Successfully unloaded module!\n");
}

module_init(initialize);
module_exit(unload);
MODULE_LICENSE("GPL");
