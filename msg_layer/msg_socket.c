/*
 * msg_socket.c - Kernel Module for Popcorn Messaging Layer over Socket
 * on Linux 4.4 for multiple nodes
 *
 * TODO:
 *		  wrap up data to a single struct
 *		  2 layer pointer to 1
 *		  concurrent connection request
 *		  use accept/conn addr to determine my_nid (kernel_accept/conn)
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

#include <popcorn/stat.h>
#include "common.h"

#define PORT 30467
#define MAX_ASYNC_BUFFER	1024

/* For enq and deq */
struct pcn_kmsg_buf_item {
	struct pcn_kmsg_message *msg;
	unsigned int dest_nid;			/* dst is not in msg_hdr */
};

/* For send and recv threads */
struct pcn_kmsg_buf {
	struct pcn_kmsg_buf_item *rbuf;
	unsigned long head;
	unsigned long tail;
	spinlock_t lock;		/* for send & recv queue */
	struct semaphore q_empty;
	struct semaphore q_full;
};

struct handler_params {
	int conn_no;
	struct socket *socket;
	struct pcn_kmsg_buf buf;
};

/* send requires this info */
static struct pcn_kmsg_buf *send_buf[MAX_NUM_NODES];

static struct socket *sock_listen;

static struct socket *sockets[MAX_NUM_NODES];
static struct task_struct *send_handlers[MAX_NUM_NODES];
static struct task_struct *recv_handlers[MAX_NUM_NODES];

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
	size = kernel_recvmsg(sock, &msg, &iov, 1, len, MSG_WAITALL);
	return size;
}

/* now is polling, not using this function right now
 * will be replaced with enq_send()
 */
static int enq_send(struct pcn_kmsg_buf *buf,
					struct pcn_kmsg_message *msg,
					unsigned int dest_nid)
{
    int err;
    unsigned long at;
	do {
		err = down_interruptible(&buf->q_full);
	} while (err);

	spin_lock(&buf->lock);
    at = buf->head;
    buf->rbuf[at].msg = msg;
    buf->rbuf[at].dest_nid = dest_nid;
    buf->head = (at + 1) & (MAX_ASYNC_BUFFER - 1);
    spin_unlock(&buf->lock);

    up(&buf->q_empty);

    return at;
}

static int deq_send(struct pcn_kmsg_buf * buf)
{
	char *p;
    unsigned long from;
    int err, dest_nid, remaining;
	struct pcn_kmsg_buf_item *msg_qitem;

	do {
		err = down_interruptible(&buf->q_empty);
	} while (err);

    spin_lock(&buf->lock);
    from = buf->tail;
    msg_qitem = buf->rbuf + from;
    buf->tail = (from + 1) & (MAX_ASYNC_BUFFER - 1);
	spin_unlock(&buf->lock);

	dest_nid = msg_qitem->dest_nid;
	p = (char *)msg_qitem->msg;
	remaining = msg_qitem->msg->header.size;

    up(&(buf->q_full));     //send q_empty++

	/* Is serialized */
	while (remaining > 0) {
		int sent = ksock_send(sockets[dest_nid], p, remaining);
		if (sent < 0) {
			MSGDPRINTK("%s: sent size < 0\n", __func__);
			io_schedule();
			continue;
		}
		p += sent;
		remaining -= sent;
		MSGDPRINTK("Sent %d remaining %d\n", sent, remaining);
	}
	pcn_kmsg_free_msg(msg_qitem->msg);
    return 0;
}

static int send_handler(void* arg0)
{
	struct handler_params *handler_params = arg0;
	int conn_no = handler_params->conn_no;
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
		MSGDPRINTK("[%d] Connecting to %pI4\n", conn_no, ip_table + conn_no);
		do {
			err = kernel_connect(sockets[conn_no],
						(struct sockaddr *)&addr, sizeof(addr), 0);
			if (err < 0) {
				MSGDPRINTK("Failed to connect the socket %d. Attempt again!!\n",
						err);
				msleep(1000);
			}
		} while (err < 0);
		complete(&connected[conn_no]);
	} else if (conn_no > my_nid) {
		wait_for_completion(&accepted[conn_no]);
	} else if (conn_no == my_nid) {
	}

	set_popcorn_node_online(conn_no, true);

	MSGPRINTK("[%d] PCN_SEND handler is up\n", conn_no);

    for (;;) {
        err = deq_send(&handler_params->buf);
    }

	return 0;
}

/*
 * buf is per conn
 */

static void deq_recv(struct pcn_kmsg_buf *buf, void *_msg, int conn_no)
{
	struct pcn_kmsg_message *msg = _msg;
	pcn_kmsg_process(msg);
}

static int recv_handler(void* arg0)
{
	int err;
	struct handler_params *handler_params = arg0;
	int conn_no = handler_params->conn_no;

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

	MSGPRINTK("[%d] PCN_RECV handler is up\n", conn_no);

	set_popcorn_node_online(conn_no, true);
	while (!kthread_should_stop()) {
		/* TODO: make a function */
		int len;
		int ret;
		size_t offset;
		struct pcn_kmsg_hdr header;
		char *data;

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
			MSGDPRINTK("(hdr) recv %d in %lu remain %d\n",
					ret, sizeof(struct pcn_kmsg_hdr), len);
		}
		MSGPRINTK("RcvH %d, %d %ld\n", conn_no, header.type, offset);

		//- compose body -//
		BUG_ON(header.type < 0 || header.size < 0 ||
					header.size > sizeof(struct pcn_kmsg_message) ||
					header.type >= PCN_KMSG_TYPE_MAX);

		data = pcn_kmsg_alloc_msg(header.size);
		BUG_ON(!data && "Unable to alloc a message");

		memcpy(data, &header, sizeof(header));

		offset = sizeof(header);
		len = header.size - offset;

		MSGDPRINTK ("(info) data size %d\n", len);

		//- data -//
		while (len > 0) {
			ret = ksock_recv(sockets[conn_no], data + offset, len);
			if (ret == -1)
				continue;
			offset += ret;
			len -= ret;
			MSGDPRINTK("(body) recv %d remain %d\n", ret, len);
		}
		MSGPRINTK("RecB %d, %d %d\n", conn_no, header.type, header.size);

		deq_recv(&handler_params->buf, data, conn_no);
	}
exit:
	sock_release(sockets[conn_no]);
	sockets[conn_no] = NULL;
end:
	sock_release(sock_listen);
	return err;
}


/***********************************************
 * This is the interface for message layer
 ***********************************************/
static int sock_kmsg_send(unsigned int dest_nid,
						struct pcn_kmsg_message *lmsg, unsigned int size)
{
	struct pcn_kmsg_message *msg;

	lmsg->header.size = size;
	lmsg->header.from_nid = my_nid;

	msg = pcn_kmsg_alloc_msg(size);
	BUG_ON(!msg);
	memcpy(msg, lmsg, size);

	enq_send(send_buf[dest_nid], msg, dest_nid);

	MSGPRINTK("%s(): sent %d, %d %d\n", __func__,
				dest_nid, lmsg->header.type, size);
	return 0;
}


static int __init initialize(void)
{
	int i, err, sender;
	struct sockaddr_in addr;

	MSGPRINTK("--- Popcorn messaging layer init starts ---\n");

	if (!identify_myself()) return -EINVAL;

	pcn_kmsg_layer_type = PCN_KMSG_LAYER_TYPE_SOCKET;
	pcn_kmsg_send_ftn = (send_ftn)sock_kmsg_send;

	for (i = 0; i < MAX_NUM_NODES; i++) {
		init_completion(&connected[i]);
		init_completion(&accepted[i]);
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

	set_popcorn_node_online(my_nid, true);
	MSGDPRINTK("Listen to the port %d\n", PORT);

	for (sender = 0; sender < 2; sender++) {
		for (i = 0; i < MAX_NUM_NODES; i++) {
			struct handler_params* param;
			char handler_name[40];
			struct task_struct *handler;
			struct pcn_kmsg_buf *pb;

			if (i == my_nid)
				continue;

			param = kmalloc(sizeof(*param), GFP_KERNEL);
			BUG_ON(!param);

			param->conn_no = i;
			pb = &param->buf;

			pb->rbuf = kmalloc(sizeof(*pb->rbuf) * MAX_ASYNC_BUFFER, GFP_KERNEL);
			BUG_ON(!(pb->rbuf));

			spin_lock_init(&pb->lock);
			pb->head = 0;
			pb->tail = 0;

			sema_init(&pb->q_empty, 0);
			sema_init(&pb->q_full, MAX_ASYNC_BUFFER);

			if (sender)
				send_buf[i] = pb;
			smp_wmb();

			sprintf(handler_name, "pcn_%s_%d", sender ? "send" : "recv", i);
			handler = kthread_run(
					sender ? send_handler : recv_handler, param, handler_name);
			if (IS_ERR(handler)) {
				printk(KERN_ERR "Handler creation failed!\n");
				return -PTR_ERR(handler);
			}

			if (sender) {
				send_handlers[i] = handler;
			} else {
				recv_handlers[i] = handler;
			}

			// TODO: support prioritized msg handler
			//struct sched_param param = {.sched_priority = 10};

			//sched_setscheduler(recv_handlers[i], SCHED_FIFO, &param);
			//set_cpus_allowed_ptr(recv_handlers[i], cpumask_of(i%NR_CPUS));
		}
	}

	/* Wait for all connections done */
	for (i = 0; i < MAX_NUM_NODES; i++) {
		if (i == my_nid) continue;
		while (!get_popcorn_node_online(i)) {
			msleep(10);
		}
		notify_my_node_info(i);
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

	MSGPRINTK("Release threads\n");
	for (i = 0; i < MAX_NUM_NODES; i++) {
		if (send_handlers[i] != NULL)
			kthread_stop(send_handlers[i]);
		if (recv_handlers[i] != NULL)
			kthread_stop(recv_handlers[i]);
		//TODO: sock release buffer, check(according to) the init
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
