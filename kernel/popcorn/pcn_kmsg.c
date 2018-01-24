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

enum pcn_kmsg_layer_types pcn_kmsg_layer_type = PCN_KMSG_LAYER_TYPE_UNKNOWN;
EXPORT_SYMBOL(pcn_kmsg_layer_type);

pcn_kmsg_cbftn pcn_kmsg_cbftns[PCN_KMSG_TYPE_MAX] = { NULL };
EXPORT_SYMBOL(pcn_kmsg_cbftns);

send_ftn pcn_kmsg_send_ftn = NULL;
EXPORT_SYMBOL(pcn_kmsg_send_ftn);

request_rdma_ftn pcn_kmsg_request_rdma_ftn = NULL;
EXPORT_SYMBOL(pcn_kmsg_request_rdma_ftn);

respond_rdma_ftn pcn_kmsg_respond_rdma_ftn = NULL;
EXPORT_SYMBOL(pcn_kmsg_respond_rdma_ftn);

free_ftn pcn_kmsg_free_ftn = NULL;
EXPORT_SYMBOL(pcn_kmsg_free_ftn);

/* Initialize callback table to null, set up control and data channels */
int __init pcn_kmsg_init(void)
{
	return 0;
}

int pcn_kmsg_register_callback(enum pcn_kmsg_type type, pcn_kmsg_cbftn callback)
{
	if (type < 0 || type >= PCN_KMSG_TYPE_MAX)
		return -ENODEV; /* invalid type */

	MSGPRINTK("%s: %d %p\n", __func__, type, callback);
	pcn_kmsg_cbftns[type] = callback;
	return 0;
}

int pcn_kmsg_unregister_callback(enum pcn_kmsg_type type)
{
	return pcn_kmsg_register_callback(type, (pcn_kmsg_cbftn)NULL);
}

int pcn_kmsg_send(unsigned int to, void *msg, unsigned int size)
{
	struct pcn_kmsg_message *m = msg;

	if (pcn_kmsg_send_ftn == NULL) {
		printk(KERN_ERR "No send function registered. "
				"Make sure the msg_layer is properly loaded");
		return -ENOENT;
	}

	BUG_ON(!msg);
	BUG_ON(m->header.type < 0 || m->header.type >= PCN_KMSG_TYPE_MAX);
	BUG_ON(size > PCN_KMSG_MAX_SIZE);
	BUG_ON(to < 0 || to >= MAX_POPCORN_NODES);
	BUG_ON(to == my_nid);

#ifdef CONFIG_POPCORN_STAT
	account_pcn_message_sent(msg);
#endif
	return pcn_kmsg_send_ftn(to, (struct pcn_kmsg_message *)msg, size);
}

void *pcn_kmsg_alloc_msg(size_t size)
{
	return kmalloc(size, GFP_KERNEL);
}

void pcn_kmsg_free_msg(void *msg)
{
	if (pcn_kmsg_free_ftn) {
		pcn_kmsg_free_ftn(msg);
	} else {
		kfree(msg);
	}
}

/*
 * Your request must be allocated by kmalloc().
 * rw_size: Max size you expect remote to perform a R/W
 */
void *pcn_kmsg_request_rdma(unsigned int to, void *msg,
						unsigned int msg_size, unsigned int rw_size)
{
	BUG_ON(to == my_nid);

#ifdef CONFIG_POPCORN_STAT
	account_pcn_message_sent(msg);
#endif
    return pcn_kmsg_request_rdma_ftn(to, msg, msg_size, rw_size);
}

void pcn_kmsg_respond_rdma(void *msg, void *paddr, u32 rw_size)
{
#ifdef CONFIG_POPCORN_STAT
	account_pcn_message_sent((struct pcn_kmsg_message *)paddr);
#endif
	pcn_kmsg_respond_rdma_ftn(msg, paddr, rw_size);
}

EXPORT_SYMBOL(pcn_kmsg_alloc_msg);
EXPORT_SYMBOL(pcn_kmsg_free_msg);
EXPORT_SYMBOL(pcn_kmsg_request_rdma);
EXPORT_SYMBOL(pcn_kmsg_respond_rdma);
EXPORT_SYMBOL(pcn_kmsg_send);
EXPORT_SYMBOL(pcn_kmsg_unregister_callback);
EXPORT_SYMBOL(pcn_kmsg_register_callback);
