/*
 * pcn_kmesg.c - Kernel Module for Popcorn Messaging Layer over Socket
 */

#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/vmalloc.h>
#include <linux/slab.h>

#include <popcorn/pcn_kmsg.h>
#include <popcorn/debug.h>
#include <popcorn/stat.h>
#include <popcorn/bundle.h>

static pcn_kmsg_cbftn pcn_kmsg_cbftns[PCN_KMSG_TYPE_MAX] = { NULL };

static struct pcn_kmsg_transport *transport = NULL;

void pcn_kmsg_set_transport(struct pcn_kmsg_transport *tr)
{
	if (transport) {
	}
	transport = tr;
}
EXPORT_SYMBOL(pcn_kmsg_set_transport);

int pcn_kmsg_register_callback(enum pcn_kmsg_type type, pcn_kmsg_cbftn callback)
{
	BUG_ON(type < 0 || type >= PCN_KMSG_TYPE_MAX);

	pcn_kmsg_cbftns[type] = callback;
	return 0;
}
EXPORT_SYMBOL(pcn_kmsg_register_callback);

int pcn_kmsg_unregister_callback(enum pcn_kmsg_type type)
{
	return pcn_kmsg_register_callback(type, (pcn_kmsg_cbftn)NULL);
}
EXPORT_SYMBOL(pcn_kmsg_unregister_callback);


void pcn_kmsg_process(struct pcn_kmsg_message *msg)
{
	pcn_kmsg_cbftn ftn;

	BUG_ON(msg->header.type < 0 || msg->header.type >= PCN_KMSG_TYPE_MAX);
	BUG_ON(msg->header.size < 0 || msg->header.size > PCN_KMSG_MAX_SIZE);

	ftn = pcn_kmsg_cbftns[msg->header.type];

	if (ftn != NULL) {
		ftn(msg);
	} else {
		printk(KERN_ERR"No callback registered for %d\n", msg->header.type);
		pcn_kmsg_done(msg);
	}

	account_pcn_message_recv(msg);
}
EXPORT_SYMBOL(pcn_kmsg_process);


static inline int __build_and_check_msg(enum pcn_kmsg_type type, int to, void *_msg, size_t size)
{
	struct pcn_kmsg_message *msg = _msg;

#ifdef CONFIG_POPCORN_CHECK_SANITY
	BUG_ON(type < 0 || type >= PCN_KMSG_TYPE_MAX);
	BUG_ON(size > PCN_KMSG_MAX_SIZE);
	BUG_ON(to < 0 || to >= MAX_POPCORN_NODES);
	BUG_ON(to == my_nid);
#endif

	msg->header.type = type;
	msg->header.prio = PCN_KMSG_PRIO_NORMAL;
	msg->header.size = size;
	msg->header.from_nid = my_nid;
	return 0;
}

int pcn_kmsg_send(enum pcn_kmsg_type type, int to, void *msg, size_t size)
{
	int ret;
	if ((ret = __build_and_check_msg(type, to, msg, size))) return ret;

	account_pcn_message_sent(msg);
	return transport->send_fn(to, msg, size);
}
EXPORT_SYMBOL(pcn_kmsg_send);

int pcn_kmsg_post(enum pcn_kmsg_type type, int to, void *msg, size_t size)
{
	int ret;
	if ((ret = __build_and_check_msg(type, to, msg, size))) return ret;

	account_pcn_message_sent(msg);
	return transport->post_fn(to, msg, size);
}
EXPORT_SYMBOL(pcn_kmsg_post);


/*
 * @res_size: The maximum size expected
 */
void *pcn_kmsg_request_rdma(enum pcn_kmsg_type type, int to, void *msg, size_t msg_size, size_t res_size)
{
	int ret;
	if ((ret = __build_and_check_msg(type, to, msg, msg_size))) return NULL;

	account_pcn_message_sent(msg);
    return transport->request_rdma_fn(to, msg, msg_size, res_size);
}
EXPORT_SYMBOL(pcn_kmsg_request_rdma);

void pcn_kmsg_respond_rdma(enum pcn_kmsg_type type, void *req, void *res, size_t res_size)
{
	int ret = __build_and_check_msg(type, PCN_KMSG_FROM_NID(req), res, res_size);
	if (ret) return;

	account_pcn_message_sent(res);
	transport->respond_rdma_fn(req, res, res_size);
}
EXPORT_SYMBOL(pcn_kmsg_respond_rdma);


void *pcn_kmsg_get(size_t size)
{
	if (transport && transport->get_fn)
		return transport->get_fn(size);
	return kmalloc(size, GFP_KERNEL);
}
EXPORT_SYMBOL(pcn_kmsg_get);

void pcn_kmsg_put(void *msg)
{
	if (transport && transport->put_fn) {
		transport->put_fn(msg);
	} else {
		kfree(msg);
	}
}
EXPORT_SYMBOL(pcn_kmsg_put);


void pcn_kmsg_done(void *msg)
{
	if (transport && transport->done_fn) {
		transport->done_fn(msg);
	} else {
		kfree(msg);
	}
}
EXPORT_SYMBOL(pcn_kmsg_done);

/* Initialize callback table to null, set up control and data channels */
int __init pcn_kmsg_init(void)
{
	return 0;
}
