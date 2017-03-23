/*
 * pcn_kmesg.c - Kernel Module for Popcorn Messaging Layer over Socket
 * on Linux 4.4 for multiple nodes
 *
 * msg layer multi-version
 * msg sent to data_sock[conn_no] according to dest_cpu
 *
 * TODO:	clean up #include
 *		  warp up data to a single struct
 *		  2 layer pointer to 1
 *		  concurrent connection request
 *		  use accpt/conn addr to determine my_nid (kernel_accept/conn)
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/semaphore.h>

#include <linux/net.h>
#include <linux/tcp.h>
#include <linux/in.h>
#include <asm/uaccess.h>
#include <linux/socket.h>

#include <linux/delay.h>
#include <linux/time.h>
#include <asm/atomic.h>
#include <linux/completion.h>

#include <linux/vmalloc.h>

/* geting host ip */
#include <linux/netdevice.h>
#include <linux/inetdevice.h>

#include <popcorn/pcn_kmsg.h>
#include <popcorn/debug.h>

#include "common.h"

char *net_dev_names[] = {
	"eth0",		// Socket
	"ib0",		// InfiniBand
	"p7p1",		// Xgene (ARM)
};

uint32_t ip_table[] = {
	IP_TO_UINT32(10, 0, 0, 100),
	IP_TO_UINT32(10, 0, 0, 101),
	IP_TO_UINT32(10, 0, 0, 102),
};

/* sock definitions */
#define PORT 1000
#define MAX_ASYNC_BUFFER  1024 // num of rbuffer for each conn

// kernel thread for waiting signal and then deq()
static int connect_thread(void *arg0);
static int accept_handler(void* arg0);

// sock_knsg_send_long(): triggered by user, doing enq() and then sending signal
static int sock_kmsg_send_long(unsigned int dest_cpu,
				struct pcn_kmsg_long_message *lmsg, unsigned int payload_size);

//** sock init **//
static struct socket *sock_data[MAX_NUM_NODES];
static struct socket *sock_listen;

//** Popcorn utility **//
static int ksock_send(struct socket *sock, char *buf, int len);
static int ksock_recv(struct socket *sock, char *buf, int len);
static uint32_t get_host_ip(char **name_ret);

//save number (MAX_ASYNC_BUFFER) of pcn_kmsg_buf
static struct pcn_kmsg_buf *send_buf[MAX_NUM_NODES];
static struct pcn_kmsg_buf *recv_buf[MAX_NUM_NODES];

struct pcn_kmsg_buf_item {
	struct pcn_kmsg_long_message *msg;
	unsigned int dest_cpu;
	unsigned int payload_size;
};

struct pcn_kmsg_buf { // we got 1024 this // kmalloc()ed
	struct pcn_kmsg_buf_item *rbuf;
	unsigned long head;
	unsigned long tail;
	spinlock_t enq_buf_mutex;
	spinlock_t deq_buf_mutex;
	struct semaphore q_empty;
	struct semaphore q_full;
};

typedef struct _send_thread_data {
	int conn_no;
	struct pcn_kmsg_buf *buf;
} send_thread_data;

typedef struct _conn_thread_data {
	int conn_no;
	struct pcn_kmsg_buf *buf;
} conn_thread_data;

typedef struct _exec_thread_data {
	int conn_no;
	struct pcn_kmsg_buf *buf;
} exec_thread_data;

/* for debug */
// check POPCORN_MSG_LAYER_VERBOSE in <linux/pcn_kmsg.h>
#define MSG_TEST	0
static struct task_struct *handler[MAX_NUM_NODES];
static struct task_struct *sender_handler[MAX_NUM_NODES];
static struct task_struct *execution_handler[MAX_NUM_NODES];
#ifdef CONFIG_POPCORN_DEBUG_MSG_LAYER_VERBOSE
static unsigned long dbg_ticket[MAX_NUM_NODES];
#endif

/* sync */
static struct mutex mutex_sock_data[MAX_NUM_NODES];
static struct completion send_completion[MAX_NUM_NODES];
static struct completion recv_completion[MAX_NUM_NODES];
static struct semaphore connect_sem[MAX_NUM_NODES];  // TODO: use wait&comp
static struct semaphore accept_sem[MAX_NUM_NODES];   // TODO: use wait&comp

/* example code */
typedef struct {
	struct pcn_kmsg_hdr header; // must followd
	// you define
	int example1;
	int example2;
}__attribute__((packed)) remote_thread_first_test_request_t; // for cache

static void handle_remote_thread_first_test_request(
								struct pcn_kmsg_long_message* inc_msg)
{
	/*  if you wanna do pong, plz remember you have to have another
		struct remote_thread_first_test_response_t */
	remote_thread_first_test_request_t *request =
			(remote_thread_first_test_request_t *)inc_msg;
	int tmp;

	tmp = request->header.from_cpu;
	MSGPRINTK("<<<<< Jack MSG_LAYER SELF-TESTING: "
			"my_nid=%d from_cpu=%d example1=%d example2=%d >>>>>\n",
			my_nid, request->header.from_cpu,
			request->example1, request->example2);

	/* extra examples */
	/* sync */
	//down_read(&mm_data->kernel_set_sem);

	/* create your bf new work */
	//INIT_WORK( (struct work_struct*)request_work,
	//							  process_count_request);
	//queue_work(exit_wq, (struct work_struct*) request_work);

	/* inc_msg is freed by messaging layer */
	return;
}

static int ksock_send(struct socket *sock, char *buf, int len)
{
	struct msghdr msg;
	struct kvec iov;
	int size;

	iov.iov_base = buf;
	iov.iov_len = len;

	msg.msg_flags = 0;
	msg.msg_control = NULL;
	msg.msg_controllen = 0;
	msg.msg_name = NULL;
	msg.msg_namelen = 0;

	// TODO: loop should be here
	size = kernel_sendmsg(sock, &msg, &iov, 1, len);
	return size;
}

static int ksock_recv(struct socket *sock, char *buf, int len)
{
	struct msghdr msg;
	struct kvec iov;
	int size;

	iov.iov_base = buf;
	iov.iov_len = len;

	msg.msg_flags   = 0;
	msg.msg_control = NULL;
	msg.msg_controllen = 0;
	msg.msg_name = NULL;
	msg.msg_namelen = 0;

	// TODO: loop should be here
	size = kernel_recvmsg(sock, &msg, &iov, 1, len, 0);
	return size;
}

/* now is polling, not using this function right now
 * will be replaced with enq_send()
 */
/*
static int enq_send(struct pcn_kmsg_buf *buf, struct pcn_kmsg_long_message *msg,
				unsigned int dest_cpu, unsigned int payload_size, int conn_no)
{
	int err;
	unsigned long head;
	MSGDPRINTK("msg_socket: enq_send-1 not used right now\n");

	err = down_interruptible(&(buf->q_full));
	if (err != 0) // testing - 0:correct others:wrong
		return err;
	spin_lock(&(buf->enq_buf_mutex));
	head = buf->head;
	//unsigned long tail = ACCESS_ONCE(buf->tail); // unused

	buf->rbuf[head].msg = (struct pcn_kmsg_buf_item*) msg; // testing
	buf->rbuf[head].dest_cpu = dest_cpu;
	buf->rbuf[head].payload_size = payload_size;
	smp_wmb();
	buf->head = (head + 1) & (MAX_ASYNC_BUFFER - 1);
	up(&(buf->q_empty));	//send q_empty++
	spin_unlock(&(buf->enq_buf_mutex));

	//complete(&send_completion[dest_cpu]);  // JACK: send to dest sock
	return 0;
}

static int deq_send(struct pcn_kmsg_buf * buf, int conn_no)
{
	int err;
	struct pcn_kmsg_buf_item msg;
	if (!is_popcorn_node_online(conn_no)) { // if still waiting for connecting
		return -1;
	}

	MSGDPRINTK("msg_socket: deq_send-1 conn_no=%d\n", conn_no);
	wait_for_completion(&send_completion[conn_no]); // Jack: make it with using completion
	err = down_interruptible(&(buf->q_empty));	  // Jack: TODO: check whethere it's needed since here can be concurrently executed
	if (err != 0) // testing - 0:correct others:wrong
		return err;

	spin_lock(&(buf->deq_buf_mutex));			   // Jack: TODO: check whethere it's needed since the same reason
	//unsigned long head = ACCESS_ONCE(buf->head);  // unused
	unsigned long tail = buf->tail;

	smp_read_barrier_depends();
	msg = buf->rbuf[tail];
	smp_mb();
	buf->tail = (tail + 1) & (MAX_ASYNC_BUFFER - 1);
	up(&(buf->q_full));	 //send q_empty++
	spin_unlock(&(buf->deq_buf_mutex));

	//TODO: Jack: NOT YET DONE!!!!!!!! do real send here!!!!!!!!!!!!!!!!!!
	//_pcn_do_send(msg.dest_cpu, msg.msg, msg.payload_size, conn_no);
	//sock_kmsg_send_long(msg.dest_cpu, msg.msg, msg.payload_size, conn_no); //JACK: this is wen's version in progress,  SO CHECK
	// wrogng this is for user

	//pcn_kmsg_free_msg(msg.msg); // must be freed by programmer
	return 0;
}
*/

static int enq_recv(struct pcn_kmsg_buf *buf,
					struct pcn_kmsg_long_message *msg, int conn_no)
{
	int err;
	unsigned long head;
	//MSGDPRINTK("msg_socket: enq_recv-1 conn_no=%d\n", conn_no);

	err = down_interruptible(&(buf->q_full)); // 0 will wait
	if (err != 0) // testing - 0:correct others:wrong
		return err;

	//spin_lock(&(buf->enq_buf_mutex));
	spin_lock(&(buf->deq_buf_mutex));
	head = buf->head;
	//unsigned long tail = ACCESS_ONCE(buf->tail); // unused
	buf->rbuf[head].msg = (struct pcn_kmsg_long_message*)msg;
	smp_wmb();
	buf->head = (head + 1) & (MAX_ASYNC_BUFFER - 1);
	up(&(buf->q_empty));
	//spin_unlock(&(buf->enq_buf_mutex));
	spin_unlock(&(buf->deq_buf_mutex));

	complete(&recv_completion[conn_no]);
	//MSGDPRINTK("msg_socket: enq_recv-2 conn_no=%d done\n", conn_no);
	return 0;
}


/*
 * buf is per conn
 */
static int deq_recv(struct pcn_kmsg_buf *buf, int conn_no)
{
	int err;
	unsigned long tail;
	struct pcn_kmsg_buf_item msg;
	pcn_kmsg_cbftn ftn;

	if (!is_popcorn_node_online(conn_no)) {
		return -1;
	}

	//MSGDPRINTK("msg_socket: deq_recv-1 conn_no=%d waiting...\n", conn_no);
	// TODO: replace spinlock with wait_for_complete. or
	//		  wait_for_complete_interruptable() // really?
	wait_for_completion(&recv_completion[conn_no]); //TODO: the same as above
	//MSGDPRINTK("msg_socket: deq_recv-2 got a signal\n");

	err = down_interruptible(&(buf->q_empty));
	if (err != 0) // testing - 0:correct others:wrong
		return err;
	spin_lock(&(buf->deq_buf_mutex));
	//MSGDPRINTK("msg_socket: deq_recv-3 CS\n");
	//unsigned long head = ACCESS_ONCE(buf->head); // unused
	tail = buf->tail;
	smp_read_barrier_depends();
	msg = buf->rbuf[tail];
	smp_mb();
	buf->tail = (tail + 1) & (MAX_ASYNC_BUFFER - 1);
	up(&(buf->q_full));
	spin_unlock(&(buf->deq_buf_mutex));

	if (msg.msg->header.type < 0 ||
				msg.msg->header.type >= PCN_KMSG_TYPE_MAX) {
		printk(KERN_INFO "Received invalid message type %d\n",
											msg.msg->header.type);
	} else {
		ftn = callbacks[msg.msg->header.type];
		if (ftn != NULL) {
			ftn((void*)msg.msg);
		} else {
			printk(KERN_INFO "Recieved message type %d size %d "
								"has no registered callback!\n",
							msg.msg->header.type,msg.msg->header.size);
		}
	}
	return 0;
}

//Jack utility -
static uint32_t get_host_ip(char **name_ret)
{
	struct net_device *device;
	struct in_device *in_dev;
	struct in_ifaddr *if_info;
	char *name;
	int i;

	__u8 *addr;

	//sprintf(net_dev_name,"eth%d",ethernet_id);  // =strcat()
	for (i = 0; i < sizeof(net_dev_names) / sizeof(*net_dev_names); i++) {
		name = net_dev_names[i];
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
	MSGPRINTK(KERN_ERR "msg_socket: ERROR - cannot find host ip\n");
	return -1;
}

// Initialize callback table to null, set up control and data channels
static int __init initialize(void)
{
	int i, err;
	char *name;
	struct sockaddr_in serv_addr;
	const uint32_t my_ip = get_host_ip(&name);
	conn_thread_data* conn_data;
	send_thread_data* send_data;

	//TODO: check how to assign a priority to these threads!
	//	  make msg_layer faster (higher prio)
	//struct sched_param param = {.sched_priority = 10};
	MSGPRINTK("--- Popcorn messaging layer init starts ---\n");

	// register callback. also define in <linux/pcn_kmsg.h>
	pcn_kmsg_register_callback(PCN_KMSG_TYPE_TEST,  // ping -
					(pcn_kmsg_cbftn)handle_remote_thread_first_test_request);
	send_callback = (send_cbftn)sock_kmsg_send_long;

	for (i = 0; i < MAX_NUM_NODES; i++) {
		if (my_ip == ip_table[i])  {
			my_nid = i;
			MSGPRINTK("Device \"%s\" my_nid=%d on machine IP %u.%u.%u.%u\n",
												name, my_nid,
												(ip_table[i]>>24)&0x000000ff,
												(ip_table[i]>>16)&0x000000ff,
												(ip_table[i]>> 8)&0x000000ff,
												(ip_table[i]>> 0)&0x000000ff);
			break;
		}
	}
	BUG_ON(my_nid < 0);

	smp_mb(); // Jack: since my_nid is extern (global)
	MSGPRINTK("----------------------------------------------------------\n");
	MSGPRINTK("----- updating to my_nid=%d wait for a moment ----\n", my_nid);
	MSGPRINTK("----------------------------------------------------------\n");
	MSGDPRINTK("MSG_LAYER: Initialization my_nid=%d\n", my_nid);

	for (i = 0; i < MAX_NUM_NODES; i++) {
		init_completion(&send_completion[i]); // Jack:
		init_completion(&recv_completion[i]); // Jack:

		sema_init(&connect_sem[i],0);  // for waiting connection established
		sema_init(&accept_sem[i],0);   // for waiting connection established

		mutex_init(&mutex_sock_data[i]);
#ifdef CONFIG_POPCORN_DEBUG_MSG_LAYER_VERBOSE
		dbg_ticket[i] = 0;
#endif
	}

	/* Initilaize the sock */
	/*
	 *  Each node has a connection table like tihs:
	 * --------------------------------------------------------------------
	 * | connect | connect | (many)... | my_nid(one) | accept | (many)... |
	 * --------------------------------------------------------------------
	 * my_nid:  no need to talk to itself
	 * connect: connecting to existing nodes
	 * accept:  waiting for the connection requests from later nodes
	 */
	// 0. init sock listening port // make a func // connect , [my_nid], accept
	sock_listen = kmalloc(sizeof(*sock_listen), GFP_KERNEL);
	err = sock_create(PF_INET, SOCK_STREAM, IPPROTO_TCP, &sock_listen);
	if (err < 0) {
		printk(KERN_ERR "Failed to create socket..!! "
						"Messaging layer init failed with err %d\n", err);
		return err;
	}

	serv_addr.sin_family = AF_INET;
	serv_addr.sin_addr.s_addr = INADDR_ANY;
	serv_addr.sin_port = htons(PORT);

	err = kernel_bind(sock_listen, (struct sockaddr *)&serv_addr,
		sizeof(serv_addr));
	if (err < 0) {
		printk(KERN_ERR "Failed to bind connection..!! "
									"Messaging layer init failed\n");
		sock_release(sock_listen);
		sock_listen = NULL;
		return err;
	}

	err = kernel_listen(sock_listen, MAX_NUM_NODES);
	if (err < 0) {
		printk(KERN_ERR "Failed to listen on connection..!! "
									"Messaging layer init failed\n");
		sock_release(sock_listen);
		sock_listen = NULL;
		return err;
	}

	set_popcorn_node_online(my_nid);
	smp_mb(); // Jack: MUST HAVE.
	MSGDPRINTK("server successfully listening on socket port=%d\n", PORT);

	// 1. init sock indo for conn thread (recv thread) and start it
	for (i = 0; i < MAX_NUM_NODES; i++) { // connect , my_nid, [accept]
		if (i == my_nid)
			continue;

		/* TODO: make a function */
		conn_data = kmalloc(sizeof(*conn_data), GFP_KERNEL);
		BUG_ON(!conn_data);

		conn_data->conn_no = i; // Take node 1 for example. connect0 [1] accept2
		// The circular buffer for received messages
		conn_data->buf = kmalloc(sizeof(*conn_data->buf), GFP_KERNEL);
		BUG_ON(!(conn_data->buf));
		conn_data->buf->rbuf =
					vmalloc(sizeof(*conn_data->buf->rbuf) * MAX_ASYNC_BUFFER);

		conn_data->buf->head = 0;
		conn_data->buf->tail = 0;

		sema_init(&(conn_data->buf->q_empty), 0);
		sema_init(&(conn_data->buf->q_full), MAX_ASYNC_BUFFER);
		recv_buf[i] = conn_data->buf;   // save this pointer to a global arry!
		spin_lock_init(&(conn_data->buf->enq_buf_mutex));
		spin_lock_init(&(conn_data->buf->deq_buf_mutex));

		smp_wmb();

		sender_handler[i] = kthread_run(accept_handler, conn_data, "pcn_recv");
		if (sender_handler[i] < 0) {
			printk(KERN_ERR "kthread_run failed! "
								"Messaging Layer not initialized\n");
			return (long long int)sender_handler[i];
		}

		//sched_setscheduler(handler[i], SCHED_FIFO, &param);
		//set_cpus_allowed_ptr(handler[i], cpumask_of(i));
	}

	// 2. init sock inf ofor send data thread (send thread) and start it
	//for (i = 0; i < my_nid; i++) { // [connect] , my_nid, accept
	for (i = 0; i < MAX_NUM_NODES; i++) { // [connect] , my_nid, accept
		if (i == my_nid)
			continue;

		/* TODO: make a function */
		send_data = kmalloc(sizeof(*send_data), GFP_KERNEL);
		BUG_ON(!send_data);

		send_data->conn_no = i; // Take node 1 for example. connect0 [1] accept2
		send_data->buf = kmalloc(sizeof(*send_data->buf), GFP_KERNEL);
		BUG_ON(!(send_data->buf));
		send_data->buf->rbuf =
					vmalloc(sizeof(*send_data->buf->rbuf) * MAX_ASYNC_BUFFER);

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

		handler[i] = kthread_run(connect_thread, send_data, "pcn_send");
		if (handler[i] < 0) {
			printk(KERN_ERR "kthread_run failed! "
								"Messaging Layer not initialized\n");
			return (long long int)handler[i];
		}

		//sched_setscheduler(sender_handler[i], SCHED_FIFO, &param);
		//set_cpus_allowed_ptr(sender_handler[i], cpumask_of(i%NR_CPUS));
	}

	// Jack: wait for connection done;
	// Jack: multi version will be a problem. Jack: but deq() should check it.
	// Jack: BUTTTTT send_callback are still null pointers so far.
	// Jack: So, we have to wait here!
	for (i = 0; i < MAX_NUM_NODES; i++) {
		while (!is_popcorn_node_online(i)) { //Jack: atomic is more safe
			MSGDPRINTK("waiting for popcorn_nodes[%d].is_connected\n", i);
			msleep(1000);
		}
	}

	MSGDPRINTK("msg_socket: msg_layer first broadcasts for popcorn info "
		"this soulde be launched after all connections are well prepared\n");
	MSGPRINTK("--- Popcorn messaging layer is up ---\n");

	/* Make init popcorn call */
	//_init_RemoteCPUMask(); // msg boradcast //Jack: deal w/ it later

	// compose msg - define -> alloc -> essential msg header info
	/* make a testing function */

#if 0
	remote_thread_first_test_request_t* request; // uTODO: make your own struct

	request = vmalloc(sizeof(*request));
	if (request == NULL)
		return -1;

	request->header.type = PCN_KMSG_TYPE_TEST;
	request->header.prio = PCN_KMSG_PRIO_NORMAL;
	//request->tgroup_home_cpu = tgroup_home_cpu;   // legacy
	//request->tgroup_home_id = tgroup_home_id;	 // legacy

	request->example1 = my_nid;
	request->example2 = my_nid + 100;

	// send msg - broadcast
	for (i = 0; i < MAX_NUM_NODES; i++) {
		if (my_nid == i)
			continue;
		pcn_kmsg_send_long(i, (struct pcn_kmsg_long_message*) request,
										sizeof(*request));
	}
	vfree(request);
#endif

	MSGDPRINTK(KERN_INFO"Popcorn Messaging Layer Initialized\n");
	MSGPRINTK("popcorn_node_online: \n");
	for (i = 0; i < MAX_NUM_NODES; i++)
		MSGPRINTK(" %d: %s\n", i,
					is_popcorn_node_online(i) ? "online" : "offline");

	return 0;
}

//Jack: TODO:  NEED TO BE SWITCHED TO BE SOCK
static int connect_thread(void* arg0) // for a conn_no
{
	int err = 0;
	//int val = 1; // unused
	send_thread_data *thread_data = (send_thread_data *) arg0;
	int conn_no = thread_data->conn_no;
	struct sockaddr_in dest_addr;

	MSGDPRINTK("msg_socket: %s(): connect_thread() on conn_no=%d\n",
														__func__, conn_no);

	//* sock connection init *//
	if ( conn_no < my_nid ) {
		msleep(5000);
		err = sock_create(PF_INET, SOCK_STREAM,
			IPPROTO_TCP, &(sock_data[conn_no]));
		if (err < 0) {
			MSGDPRINTK("Failed to create socket..!! "
							"Messaging layer init failed with err %d\n", err);
			return err;
		}
		dest_addr.sin_family = AF_INET;
		dest_addr.sin_port = htons(PORT);
		dest_addr.sin_addr.s_addr = htonl(ip_table[conn_no]); // target ip(diff)
		MSGDPRINTK("my_node=%d connecting to port %d on machine %u.%u.%u.%u\n",
										conn_no, PORT,
										(ip_table[conn_no]>>24)&0x000000ff,
										(ip_table[conn_no]>>16)&0x000000ff,
										(ip_table[conn_no]>> 8)&0x000000ff,
										(ip_table[conn_no]>> 0)&0x000000ff);
		do {
			err = kernel_connect(sock_data[conn_no], 
						(struct sockaddr *)&dest_addr, sizeof(dest_addr), 0);
			if (err < 0) {
				MSGDPRINTK("Failed to connect to socket..!! "
							"Messaging layer init failed with err %d\n", err);
			}
		} while (err < 0);
		up(&connect_sem[conn_no]);
		// For slave, it's connected when connection is established
	} else if ( conn_no > my_nid ) {
		MSGPRINTK("%s(): my_nid=%d waiting... for conn_no=%d done "
						"on accept_handler()\n", __func__, my_nid, conn_no);
		err = down_interruptible(&accept_sem[conn_no]);
		if (err != 0) // testing - 0:correct others:wrong
			return err;
		// For master, it's connected when a connection is accepted
	} else if ( conn_no == my_nid ) {
		MSGPRINTK("msg_socket: %s(): accept() skip myself\n", __func__);
	}
	// sock connection init done
	//up(&send_connDone[channel_num]);

	set_popcorn_node_online(conn_no); //Jack: atomic is more safe
	smp_mb(); // Jack: MUST HAVE
	MSGPRINTK("%s(): Connection Established (Done)...PCN_SEND Thread "
				"my_nid=%d conn_no=%d (GOOD)\n", __func__, my_nid, conn_no);
	return 0;
}

static int executer_thread(void* arg0)
{
	int err;
	exec_thread_data *data = (exec_thread_data *) arg0;
	int conn_no = data->conn_no;

	for (;;) {
		// TODO: change to
		err = deq_recv(recv_buf[conn_no], conn_no);
	}
	//kfree(data);
	return 0;
}

static int accept_handler(void* arg0)
{
	/* after sock_create -> bind -> listern, now waiting for new connections */
	// Jack: from sock
	int err;
	exec_thread_data *exec_data;
	conn_thread_data *conn_data = (conn_thread_data*) arg0;

	int conn_no = conn_data->conn_no;

	if (conn_no > my_nid) {
		err = sock_create(PF_INET, SOCK_STREAM, IPPROTO_TCP,
													&sock_data[conn_no]);
		if (err < 0) {
			MSGDPRINTK("Failed to create socket..!! "
							"Messaging layer init failed with err %d\n", err);
			goto end;
		}
		MSGPRINTK("msg_socket: %s(): accept()ing... conn_no=%d\n",
														__func__, conn_no);
		//IMPORTANT!!! and cannot be O_NONBLOCK
		err = kernel_accept(sock_listen, sock_data + conn_no, 0);
		if (err < 0) {
			MSGDPRINTK("Failed to accept connection..!! "
											"Messaging layer init failed\n");
			goto exit;
		}
		MSGPRINTK("msg_socket: %s(): accept() conn_no=%d DONE\n",
														__func__, conn_no);
		up(&accept_sem[conn_no]);
		// For master, it's able to receive messages when a conn is accepted
	} else if (conn_no < my_nid) {
		MSGPRINTK("%s(): my_nid=%d just waiting... for conn_no=%d done "
						"on connect_thread()\n", __func__, my_nid, conn_no);
		err = down_interruptible(&connect_sem[conn_no]);
		if (err != 0) // testing - 0:correct others:wrong
			return err;
		// For slave, it's able to receive messages when conn is established
	} else if (conn_no == my_nid) {
		MSGPRINTK("msg_socket: %s(): connection() skip myself\n", __func__);
	}
	MSGPRINTK("%s(): my_nid=%d conn_no=%d ESTABLISHED (GOOD)\n",
													__func__, my_nid, conn_no);
	exec_data = kmalloc(sizeof(*exec_data), GFP_KERNEL);
	exec_data->conn_no = conn_no;
	execution_handler[conn_no] =
					kthread_run(executer_thread, exec_data, "pcnscif_execD_pp");
	MSGPRINTK("%s(): execution damon ESTABLISHED my_nid=%d conn_no=%d (GOOD)\n",
													__func__, my_nid, conn_no);

	//* doese the polling. copy data from sock to kernel *//
	set_popcorn_node_online(conn_no); //Jack: atomic is more safe
	smp_mb(); // Jack: MUST HAVE

	while (1) { /* only single daemon doing this */
		//if (kthread_should_stop()) { //Jack: I think no need
		//	goto exit;	// check has anyone call thread_stop
		//}

		/* TODO: make a function */
		int len;
		int ret;
		size_t offset;
		struct pcn_kmsg_hdr header;
		struct pcn_kmsg_long_message *data;

		/* TODO: Jack IMPORTANT for performance !!!!!!!!!!!!!!!!!
		 * here we can use complete:
		 * the following copy from sock to kernel part uses ->
		 * wait_for_completion(&jack_deq_comeplete);  // if receive sth
		 * the callback func execution part uses -> complete() or complete_all()
		 */

		//- compose hdr in data -//
		offset = 0;
		len = sizeof(header);
		while (len > 0) {
			ret = ksock_recv(sock_data[conn_no],
					(char *)(&header) + offset, len);
			if (ret == -1)
				continue;
			offset += ret;
			len -= ret;
			MSGDPRINTK("daemon: (hdr) recv %d in %lu remain=%d\n",
					ret, sizeof(struct pcn_kmsg_hdr), len);
		}

		//- compose body -//

		data = pcn_kmsg_alloc_msg(header.size);
		BUG_ON(!data && "Unable to alloc a message");

		memcpy(data, &header, sizeof(header));

		offset = sizeof(header);
		len = header.size - offset;

		MSGDPRINTK ("daemon: (info) %lu, data(msg) size = len %d = %d-%lu\n",
					header.ticket, len, header.size, sizeof(header));

		//- data -//
		while (len > 0) {
			ret = ksock_recv(sock_data[conn_no], ((char *)data) + offset, len);
			if (ret == -1)
				continue;
			offset += ret;
			len -= ret;
			MSGDPRINTK("daemon: (body) recv %d in %lu remain=%d\n",
					ret, data->header.size - sizeof(header), len);
		}

		err = enq_recv(recv_buf[conn_no], data, conn_no);
	}

	/* all accect() are done */
exit:
	sock_release(sock_data[conn_no]);
	sock_data[conn_no] = NULL;
end:
	sock_release(sock_listen);
	sock_listen = NULL;
	return err;
}


/***********************************************
 * Jack: refering to from wen's __pcn_do_send()
 * This is a users callback function
 ***********************************************/
static int sock_kmsg_send_long(unsigned int dest_cpu,
			struct pcn_kmsg_long_message *lmsg, unsigned int payload_size)
{
	volatile int left;
	int size = 0;
	char *p;

	lmsg->header.size = payload_size;
	lmsg->header.from_cpu = my_nid;

	// Jack: multi-msg_layer - Send msg to itself
	if (dest_cpu == my_nid) {
		struct pcn_kmsg_long_message *msg = // redundant for current implement
									pcn_kmsg_alloc_msg(lmsg->header.size);
		BUG_ON(!msg);
		memcpy(msg, lmsg, lmsg->header.size);

		if (msg->header.type < 0 || msg->header.type >= PCN_KMSG_TYPE_MAX) {
			MSGDPRINTK(KERN_INFO "Received invalid message type %d\n",
															msg->header.type);
		} else {
			pcn_kmsg_cbftn ftn;
			ftn = callbacks[msg->header.type];
			if (ftn != NULL) {
				ftn((void*)msg);
			} else {
				MSGDPRINTK(KERN_INFO "Recieved message type %d size %d "
									 "has no registered callback!\n",
									 msg->header.type, msg->header.size);
			}
		}
		return 0;
	}

	mutex_lock(&mutex_sock_data[dest_cpu]);
	/* TODO: make a function */
#ifdef CONFIG_POPCORN_DEBUG_MSG_LAYER_VERBOSE
	lmsg->header.ticket = ++dbg_ticket[dest_cpu];
#endif
	// Send to others - two way to do. 1. directely send() 2 enq()
	//1.
	left = lmsg->header.size;
	p = (char *)lmsg;
	MSGDPRINTK("%s: dest_cpu=%d ticket=%lu conn_no=%d\n",
					__func__, dest_cpu, lmsg->header.ticket, dest_cpu);
	while (left > 0) {
		size = ksock_send(sock_data[dest_cpu], p, left);
		if (size < 0) {
			MSGDPRINTK("%s: send size < 0\n", __func__);
			io_schedule();
			continue;
		}
		p += size;
		left -= size;
		MSGDPRINTK ("\tsent %d in %d (total including hdr), left=%d\n",
										size, lmsg->header.size, left);
	}
	mutex_unlock(&mutex_sock_data[dest_cpu]);
	MSGDPRINTK("msg_socket: sent to %d\n", dest_cpu);
	// TODO: 2. enq() rather than directly ksock_send()
	//enq_send(struct pcn_kmsg_buf * buf,
	//			struct pcn_kmsg_long_message *msg,
	//			dest_cpu, payload_size, conn_no);

	/* end - the programmer will free lmsg */
	return 0;
}

static void __exit unload(void)
{
	int i;
	MSGPRINTK("Stopping kernel threads\n");
	/** TODO: at least a NULL a pointer below this line **/
	/* To move out of recv_queue */
	for (i = 0; i < MAX_NUM_NODES; i++) {
		complete_all(&send_completion[i]);
		complete_all(&recv_completion[i]);
	}

	// these five are local variable
	//kfree(send_data->buf);
	//kfree(conn_data->buf);
	//kfree(send_data);
	//kfree(conn_data);
	//kfree(exec_data);

	/* release */
	MSGPRINTK("Release threads\n");
	for (i = 0; i < MAX_NUM_NODES; i++) {
		if (handler[i] != NULL)
			kthread_stop(handler[i]);
		if (sender_handler[i] != NULL)
			kthread_stop(sender_handler[i]);
		if (execution_handler[i] != NULL)
			kthread_stop(execution_handler[i]);
		//Jack: TODO: sock release buffer, check(according to) the init
	}

	//TODO: release socket
	MSGPRINTK("Release sockets\n");
	sock_release(sock_listen);
	for (i = 0; i < MAX_NUM_NODES; i++) {
		if (sock_data[i] != NULL) // TODO: see if we need this
			sock_release(sock_data[i]);
	}
	//sock_release(sock_listen);

	MSGPRINTK("Successfully unloaded module!\n");
}

module_init(initialize);
module_exit(unload);
MODULE_LICENSE("GPL");
