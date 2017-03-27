/*
 * msg_socket.c - Kernel Module for Popcorn Messaging Layer over Socket
 * on Linux 4.4 for multiple nodes
 *
 * TODO:
 *		  warp up data to a single struct
 *		  2 layer pointer to 1
 *		  concurrent connection request
 *		  use accpt/conn addr to determine my_nid (kernel_accept/conn)
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/kthread.h>
#include <linux/completion.h>

#include <linux/net.h>
#include <linux/inet.h>
#include <linux/inetdevice.h>

#include <asm/uaccess.h>

#include "common.h"

char *net_dev_names[] = {
	"eth0",		// Socket
	"ib0",		// InfiniBand
	"p7p1",		// Xgene (ARM)
};

const char * const ip_addresses[] = {
	"10.0.0.100",
	"10.0.0.101",
	"10.0.0.102",
};

uint32_t ip_table[MAX_NUM_NODES] = { 0 };

#define PORT 30467
#define MAX_ASYNC_BUFFER	1024

struct pcn_kmsg_buf_item {
	struct pcn_kmsg_long_message *msg;
	unsigned int dest_nid;
	unsigned int payload_size;
};

struct pcn_kmsg_buf {
	struct pcn_kmsg_buf_item *rbuf;
	unsigned long head;
	unsigned long tail;
	spinlock_t enq_buf_mutex;
	spinlock_t deq_buf_mutex;
	struct semaphore q_empty;
	struct semaphore q_full;
};

struct handler_data {
	int conn_no;
	struct pcn_kmsg_buf *buf;
};


static struct socket *sock_listen;

static struct socket *sockets[MAX_NUM_NODES];
static struct task_struct *send_handlers[MAX_NUM_NODES];
static struct task_struct *recv_handlers[MAX_NUM_NODES];
static struct task_struct *exec_handlers[MAX_NUM_NODES];

/* for debug */
#ifdef CONFIG_POPCORN_DEBUG_MSG_LAYER_VERBOSE
static unsigned long dbg_ticket[MAX_NUM_NODES];
#endif

/* sync */
static struct mutex mutex_sockets[MAX_NUM_NODES];
static struct completion send_completion[MAX_NUM_NODES];
static struct completion recv_completion[MAX_NUM_NODES];
static struct completion connected[MAX_NUM_NODES];
static struct completion accepted[MAX_NUM_NODES];

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


static uint32_t get_host_ip(char **name_ret)
{
	int i;

	for (i = 0; i < sizeof(net_dev_names) / sizeof(*net_dev_names); i++) {
		struct net_device *device;
		struct in_device *in_dev;
		char *name = net_dev_names[i];
		device = dev_get_by_name(&init_net, name); // namespace=normale

		if (device) {
			struct in_ifaddr *if_info;

			*name_ret = name;
			in_dev = (struct in_device *)device->ip_ptr;
			if_info = in_dev->ifa_list;

			MSGDPRINTK(KERN_WARNING "Device %s IP: %p4I\n",
							name, if_info->ifa_local);
			return if_info->ifa_local;
		}
	}
	MSGPRINTK(KERN_ERR "msg_socket: ERROR - cannot find host ip\n");
	return -1;
}


static int send_handler(void* arg0)
{
	struct handler_data *handler_data = arg0;
	int conn_no = handler_data->conn_no;
	int err = 0;

	if (conn_no < my_nid) {
		struct sockaddr_in addr;
		err = sock_create(PF_INET, SOCK_STREAM,
			IPPROTO_TCP, &(sockets[conn_no]));
		if (err < 0) {
			MSGDPRINTK("Failed to create socket..!! "
							"Messaging layer init failed with err %d\n", err);
			return err;
		}
		addr.sin_family = AF_INET;
		addr.sin_port = htons(PORT);
		addr.sin_addr.s_addr = ip_table[conn_no];
		MSGDPRINTK("[%d] Connecting to %pI4\n", conn_no, iptable + conn_no);
		do {
			err = kernel_connect(sockets[conn_no],
						(struct sockaddr *)&addr, sizeof(addr), 0);
			if (err < 0) {
				MSGDPRINTK("Failed to connect the socket %d. Attempt again!!\n",
						err);
			}
		} while (err < 0);
		complete(&connected[conn_no]);
	} else if (conn_no > my_nid) {
		wait_for_completion(&accepted[conn_no]);
	} else if (conn_no == my_nid) {
		// Skip connecting to myself
	}

	set_popcorn_node_online(conn_no);

	MSGPRINTK("[%d] PCN_SEND handler is up\n", conn_no);
	return 0;
}


/*
 * buf is per conn
 */
static int enq_recv(struct pcn_kmsg_buf *buf,
					struct pcn_kmsg_long_message *msg, int conn_no)
{
	int err;
	unsigned long head;

	err = down_interruptible(&(buf->q_full));
	if (err != 0)
		return err;

	spin_lock(&(buf->deq_buf_mutex));
	head = buf->head;
	buf->rbuf[head].msg = (struct pcn_kmsg_long_message*)msg;
	smp_wmb();
	buf->head = (head + 1) & (MAX_ASYNC_BUFFER - 1);
	up(&(buf->q_empty));
	spin_unlock(&(buf->deq_buf_mutex));

	complete(&recv_completion[conn_no]);
	return 0;
}


static int deq_recv(struct pcn_kmsg_buf *buf, int conn_no)
{
	int err;
	unsigned long tail;
	struct pcn_kmsg_buf_item msg;
	pcn_kmsg_cbftn ftn;

	wait_for_completion(&recv_completion[conn_no]);

	err = down_interruptible(&(buf->q_empty));
	if (err != 0)
		return err;

	spin_lock(&(buf->deq_buf_mutex));
	tail = buf->tail;
	smp_read_barrier_depends();
	msg = buf->rbuf[tail];
	smp_mb();
	buf->tail = (tail + 1) & (MAX_ASYNC_BUFFER - 1);
	up(&(buf->q_full));
	spin_unlock(&(buf->deq_buf_mutex));

	ftn = callbacks[msg.msg->header.type];
	if (ftn != NULL) {
		ftn((void*)msg.msg);
	} else {
		printk(KERN_INFO "Recieved message type %d size %d "
							"has no registered callback!\n",
						msg.msg->header.type, msg.msg->header.size);
	}
	return 0;
}


static int exec_handler(void* arg0)
{
	int err;
	struct handler_data *data = arg0;
	int conn_no = data->conn_no;

	for (;;) {
		err = deq_recv(data->buf, conn_no);
	}
	return 0;
}


static int recv_handler(void* arg0)
{
	int err;
	struct handler_data *handler_data = arg0;

	int conn_no = handler_data->conn_no;

	if (conn_no > my_nid) {
		err = sock_create(PF_INET, SOCK_STREAM, IPPROTO_TCP,
													&sockets[conn_no]);
		if (err < 0) {
			MSGDPRINTK("Failed to create socket..!! "
							"Messaging layer init failed with err %d\n", err);
			goto end;
		}

		err = kernel_accept(sock_listen, sockets + conn_no, 0);
		if (err < 0) {
			MSGDPRINTK("Failed to accept from %d for %d\n", conn_no, err);
			goto exit;
		}
		complete(&accepted[conn_no]);
	} else if (conn_no < my_nid) {
		wait_for_completion(&connected[conn_no]);
	} else if (conn_no == my_nid) {
		// Skip connecting to myself
	}

	exec_handlers[conn_no] =
					kthread_run(exec_handler, handler_data, "pcn_exec");

	MSGPRINTK("[%d] PCN_RECV handler is up\n", conn_no);

	set_popcorn_node_online(conn_no);

	while (!kthread_should_stop()) {
		/* TODO: make a function */
		int len;
		int ret;
		size_t offset;
		struct pcn_kmsg_hdr header;
		struct pcn_kmsg_long_message *data;

		//- compose hdr in data -//
		offset = 0;
		len = sizeof(header);
		while (len > 0) {
			ret = ksock_recv(sockets[conn_no],
					(char *)(&header) + offset, len);
			if (ret == -1)
				continue;
			offset += ret;
			len -= ret;
			MSGDPRINTK("(hdr) recv %d in %lu remain=%d\n",
					ret, sizeof(struct pcn_kmsg_hdr), len);
		}

		//- compose body -//
		BUG_ON(header.type < 0 || header.type >= PCN_KMSG_TYPE_MAX);

		data = pcn_kmsg_alloc_msg(header.size);
		BUG_ON(!data && "Unable to alloc a message");

		memcpy(data, &header, sizeof(header));

		offset = sizeof(header);
		len = header.size - offset;

		MSGDPRINTK ("(info) %lu, data size %d\n", header.ticket, len);

		//- data -//
		while (len > 0) {
			ret = ksock_recv(sockets[conn_no], ((char *)data) + offset, len);
			if (ret == -1)
				continue;
			offset += ret;
			len -= ret;
			MSGDPRINTK("(body) recv %d in %lu remain=%d\n",
					ret, data->header.size - sizeof(header), len);
		}

		err = enq_recv(handler_data->buf, data, conn_no);
	}

exit:
	sock_release(sockets[conn_no]);
	sockets[conn_no] = NULL;
end:
	sock_release(sock_listen);
	sock_listen = NULL;
	return err;
}


/***********************************************
 * This is the interface for message layer
 ***********************************************/
static int sock_kmsg_send_long(unsigned int dest_nid,
			struct pcn_kmsg_long_message *lmsg, unsigned int payload_size)
{
	int remaining;
	char *p;

	BUG_ON(lmsg->header.type < 0 || lmsg->header.type >= PCN_KMSG_TYPE_MAX);

	lmsg->header.size = payload_size;
	lmsg->header.from_nid = my_nid;
#ifdef CONFIG_POPCORN_DEBUG_MSG_LAYER_VERBOSE
	lmsg->header.ticket = ++dbg_ticket[dest_nid];
#endif

	// Send msg to myself
	if (dest_nid == my_nid) {
		pcn_kmsg_cbftn ftn;
		struct pcn_kmsg_long_message *msg =
									pcn_kmsg_alloc_msg(lmsg->header.size);
		BUG_ON(!msg);
		memcpy(msg, lmsg, lmsg->header.size);

		ftn = callbacks[msg->header.type];
		if (ftn != NULL) {
			ftn((void*)msg);
		} else {
			MSGDPRINTK(KERN_INFO "Recieved message type %d size %d "
								 "has no registered callback!\n",
								 msg->header.type, msg->header.size);
			return -ENOENT;
		}
		return 0;
	}

	mutex_lock(&mutex_sockets[dest_nid]);

	remaining = lmsg->header.size;
	p = (char *)lmsg;
	MSGDPRINTK("%s: dest_nid=%d ticket=%lu conn_no=%d\n",
					__func__, dest_nid, lmsg->header.ticket, dest_nid);
	while (remaining > 0) {
		int sent = ksock_send(sockets[dest_nid], p, remaining);
		if (sent < 0) {
			MSGDPRINTK("%s: sent size < 0\n", __func__);
			io_schedule();
			continue;
		}
		p += sent;
		remaining -= sent;
		MSGDPRINTK ("Sent %d in %d. remaining=%d\n",
				sent, lmsg->header.size, remaining);
	}
	mutex_unlock(&mutex_sockets[dest_nid]);

	MSGDPRINTK("Sent to %d\n", dest_nid);

	return 0;
}


static int __init initialize(void)
{
	int i, err, sender;
	char *name;
	struct sockaddr_in addr;
	uint32_t my_ip = get_host_ip(&name);

	MSGPRINTK("--- Popcorn messaging layer init starts ---\n");

	// register callback.
	send_callback = (send_cbftn)sock_kmsg_send_long;

	for (i = 0; i < MAX_NUM_NODES; i++) {
		char *me = " ";
		init_completion(&send_completion[i]);
		init_completion(&recv_completion[i]);

		init_completion(&connected[i]);
		init_completion(&accepted[i]);

		mutex_init(&mutex_sockets[i]);
#ifdef CONFIG_POPCORN_DEBUG_MSG_LAYER_VERBOSE
		dbg_ticket[i] = 0;
#endif
		ip_table[i] = in_aton(ip_addresses[i]);

		if (my_ip == ip_table[i]) {
			my_nid = i;
			me = "*";
		}
		PRINTK(" %s %2d: %pI4\n", me, i, ip_table + i);
	}
	PRINTK("\n");

	smp_mb();
	BUG_ON(my_nid < 0);


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
	sock_listen = kmalloc(sizeof(*sock_listen), GFP_KERNEL);
	err = sock_create(PF_INET, SOCK_STREAM, IPPROTO_TCP, &sock_listen);
	if (err < 0) {
		printk(KERN_ERR "Failed to create socket..!! "
						"Messaging layer init failed with err %d\n", err);
		return err;
	}

	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = INADDR_ANY;
	addr.sin_port = htons(PORT);

	err = kernel_bind(sock_listen, (struct sockaddr *)&addr, sizeof(addr));
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
	MSGDPRINTK("Listen to the port %d\n", PORT);

	for (sender = 0; sender < 2; sender++) {
		// TODO: support prioritized msg handler
		//struct sched_param param = {.sched_priority = 10};

		for (i = 0; i < MAX_NUM_NODES; i++) {
			struct handler_data* data;
			char handler_name[80];
			struct task_struct *handler;

			if (i == my_nid)
				continue;

			/* TODO: make a function */
			data = kmalloc(sizeof(*data), GFP_KERNEL);
			BUG_ON(!data);

			data->conn_no = i;

			// The ring buffer for asynchronous amessage processing
			data->buf = kmalloc(sizeof(*data->buf), GFP_KERNEL);
			BUG_ON(!(data->buf));
			data->buf->rbuf =
						vmalloc(sizeof(*data->buf->rbuf) * MAX_ASYNC_BUFFER);
			BUG_ON(!(data->buf->rbuf));

			data->buf->head = 0;
			data->buf->tail = 0;

			sema_init(&data->buf->q_empty, 0);
			sema_init(&data->buf->q_full, MAX_ASYNC_BUFFER);
			spin_lock_init(&data->buf->enq_buf_mutex);
			spin_lock_init(&data->buf->deq_buf_mutex);
			smp_wmb();

			sprintf(handler_name, "pcn_%s%d\n", sender ? "send" : "recv", i);
			handler = kthread_run(
					sender ? send_handler : recv_handler, data, handler_name);
			if (IS_ERR(handler)) {
				printk(KERN_ERR "Handler creation failed!\n");
				return -PTR_ERR(handler);
			}

			if (sender) {
				send_handlers[i] = handler;
			} else {
				recv_handlers[i] = handler;
				//sched_setscheduler(recv_handlers[i], SCHED_FIFO, &param);
				//set_cpus_allowed_ptr(recv_handlers[i], cpumask_of(i%NR_CPUS));
			}
		}
	}

	/**
	 * wait for connection done;
	 * multi version will be a problem. Jack: but deq() should check it.
	 * BUTTTTT send_callback are still null pointers so far.
	 * So, we have to wait here!
	*/
	for (i = 0; i < MAX_NUM_NODES; i++) {
		while (!is_popcorn_node_online(i)) {
			io_schedule();
		}
	}

	MSGPRINTK("--- Popcorn messaging layer is up ---\n");

	return 0;
}


static void __exit unload(void)
{
	int i;

	BUG_ON("Actually not tested at all");

	MSGPRINTK("Stopping kernel threads\n");

	/** TODO: at least a NULL a pointer below this line **/
	/* To move out of recv_queue */
	for (i = 0; i < MAX_NUM_NODES; i++) {
		complete_all(&send_completion[i]);
		complete_all(&recv_completion[i]);
	}

	MSGPRINTK("Release threads\n");
	for (i = 0; i < MAX_NUM_NODES; i++) {
		if (send_handlers[i] != NULL)
			kthread_stop(send_handlers[i]);
		if (recv_handlers[i] != NULL)
			kthread_stop(recv_handlers[i]);
		if (exec_handlers[i] != NULL)
			kthread_stop(exec_handlers[i]);
		//Jack: TODO: sock release buffer, check(according to) the init
	}

	MSGPRINTK("Release sockets\n");
	sock_release(sock_listen);
	for (i = 0; i < MAX_NUM_NODES; i++) {
		if (sockets[i] != NULL) // TODO: see if we need this
			sock_release(sockets[i]);
	}
	sock_release(sock_listen);

	MSGPRINTK("Successfully unloaded module!\n");
}

module_init(initialize);
module_exit(unload);
MODULE_LICENSE("GPL");
