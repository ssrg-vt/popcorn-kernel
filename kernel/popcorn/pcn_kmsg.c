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

pcn_kmsg_cbftn pcn_kmsg_cbftns[PCN_KMSG_TYPE_MAX] = { NULL };
EXPORT_SYMBOL(pcn_kmsg_cbftns);

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
		pcn_kmsg_free_msg(msg);
	}

	account_pcn_message_recv(msg);
}
EXPORT_SYMBOL(pcn_kmsg_process);


#ifdef CONFIG_POPCORN_CHECK_SANITY
static void __check_kmsg_sanity(int to, struct pcn_kmsg_message *msg, size_t size)
{
	BUG_ON(msg->header.type < 0 || msg->header.type >= PCN_KMSG_TYPE_MAX);
	BUG_ON(size > PCN_KMSG_MAX_SIZE);
	BUG_ON(to < 0 || to >= MAX_POPCORN_NODES);
	BUG_ON(to == my_nid);
}
#endif

int pcn_kmsg_send(int to, void *_msg, size_t size)
{
	struct pcn_kmsg_message *msg = _msg;

#ifdef CONFIG_POPCORN_CHECK_SANITY
	if (!transport || !transport->send_fn) {
		printk(KERN_ERR "No send function registered. "
				"Make sure the msg_layer is properly loaded");
		return -EINVAL;
	}
	__check_kmsg_sanity(to, msg, size);
#endif

	msg->header.size = size;
	msg->header.from_nid = my_nid;

	account_pcn_message_sent(msg);
	return transport->send_fn(to, msg, size);
}
EXPORT_SYMBOL(pcn_kmsg_send);

int pcn_kmsg_post(int to, void *_msg, size_t size)
{
	struct pcn_kmsg_message *msg = _msg;

#ifdef CONFIG_POPCORN_CHECK_SANITY
	if (!transport || !transport->post_fn) {
		printk(KERN_ERR "No send function registered. "
				"Make sure the msg_layer is properly loaded");
		return -EINVAL;
	}
	__check_kmsg_sanity(to, msg, size);
#endif

	msg->header.size = size;
	msg->header.from_nid = my_nid;

	account_pcn_message_sent(msg);
	return transport->post_fn(to, msg, size);
}
EXPORT_SYMBOL(pcn_kmsg_post);

/*
 * @res_size: The maximum size expected
 */
void *pcn_kmsg_request_rdma(int to, void *_msg, size_t msg_size, size_t res_size)
{
	struct pcn_kmsg_message *msg = _msg;

#ifdef CONFIG_POPCORN_CHECK_SANITY
	if (!transport || !transport->request_rdma_fn) {
		printk(KERN_ERR "No rdma function registered");
		return NULL;
	}
	__check_kmsg_sanity(to, msg, msg_size);
#endif

	msg->header.size = msg_size;
	msg->header.from_nid = my_nid;

	account_pcn_message_sent(msg);
    return transport->request_rdma_fn(to, _msg, msg_size, res_size);
}
EXPORT_SYMBOL(pcn_kmsg_request_rdma);

void pcn_kmsg_respond_rdma(void *_req, void *_res, size_t res_size)
{
	struct pcn_kmsg_message *res = _res;

#ifdef CONFIG_POPCORN_CHECK_SANITY
	struct pcn_kmsg_message *req = _req;
	if (!transport || !transport->respond_rdma_fn) {
		printk(KERN_ERR "No rdma respond function");
		return;
	}
	__check_kmsg_sanity(req->header.from_nid, _res, res_size);
#endif

	res->header.size = res_size;
	res->header.from_nid = my_nid;

	account_pcn_message_sent(res);
	transport->respond_rdma_fn(_req, _res, res_size);
}
EXPORT_SYMBOL(pcn_kmsg_respond_rdma);


void *pcn_kmsg_alloc_msg(size_t size)
{
	return kmalloc(size, GFP_KERNEL);
}
EXPORT_SYMBOL(pcn_kmsg_alloc_msg);

void pcn_kmsg_free_msg(void *msg)
{
	if (transport && transport->free_fn) {
		transport->free_fn(msg);
	} else {
		kfree(msg);
	}
}
EXPORT_SYMBOL(pcn_kmsg_free_msg);


/* Initialize callback table to null, set up control and data channels */
int __init pcn_kmsg_init(void)
{
	return 0;
}
