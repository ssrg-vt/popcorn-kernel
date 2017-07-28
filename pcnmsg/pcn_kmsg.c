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

/* Message usage pattern */
#ifdef CONFIG_POPCORN_MSG_STATISTIC
atomic_t send_pattern[MAX_STATISTIC_SLOTS];
atomic_t recv_pattern[MAX_STATISTIC_SLOTS];
EXPORT_SYMBOL(send_pattern);
EXPORT_SYMBOL(recv_pattern);
#endif

/* For testing RDMA READ/WRITE */
char *dummy_act_buf;
char *dummy_pass_buf;
EXPORT_SYMBOL(dummy_act_buf);
EXPORT_SYMBOL(dummy_pass_buf);

char *msg_layer = NULL;
EXPORT_SYMBOL(msg_layer);

pcn_kmsg_cbftn callbacks[PCN_KMSG_TYPE_MAX];
EXPORT_SYMBOL(callbacks);

send_cbftn send_callback;
EXPORT_SYMBOL(send_callback);

send_rdma_cbftn send_callback_rdma;
EXPORT_SYMBOL(send_callback_rdma);

handle_rdma_request_ftn handle_rdma_callback;
EXPORT_SYMBOL(handle_rdma_callback);

/* Initialize callback table to null, set up control and data channels */
int __init pcn_kmsg_init(void)
{
#ifdef CONFIG_POPCORN_MSG_STATISTIC
	int i;
	for(i=0; i<MAX_STATISTIC_SLOTS; i++) {
		send_pattern[i].counter = 0;
        recv_pattern[i].counter = 0;
	}
#endif
	send_callback = NULL;
	MSGPRINTK("%s: done\n", __func__);
	return 0;
}

int pcn_kmsg_register_callback(enum pcn_kmsg_type type, pcn_kmsg_cbftn callback)
{
	if (type >= PCN_KMSG_TYPE_MAX)
		return -ENODEV; /* invalid type */

	MSGPRINTK("%s: %d \n", __func__, type);
	callbacks[type] = callback;
	return 0;
}

int pcn_kmsg_unregister_callback(enum pcn_kmsg_type type)
{
	if (type >= PCN_KMSG_TYPE_MAX)
		return -ENODEV;

	MSGPRINTK("%s: %d\n", __func__, type);
	callbacks[type] = NULL;
	return 0;
}

int pcn_kmsg_send(unsigned int to, void *lmsg, unsigned int size)
{
	if (send_callback == NULL) {
		struct pcn_kmsg_hdr *hdr = lmsg;

		printk(KERN_ERR"%s: No send fn. from=%u, type=%d, size=%u\n",
					__func__, hdr->from_nid, hdr->type, size);
		// msleep(100);
		//printk("Waiting for call back function to be registered\n");
		return -ENOENT;
	}

#ifdef CONFIG_POPCORN_MSG_STATISTIC
	atomic_inc(&send_pattern[size]);
#endif

	return send_callback(to, (struct pcn_kmsg_message *)lmsg, size);
}

void *pcn_kmsg_alloc_msg(size_t size)
{
	return kmalloc(size, GFP_KERNEL);
}

void pcn_kmsg_free_msg(void *msg)
{
	kfree(msg);
}

/*
 * Your request must be allocated by kmalloc().
 */
int pcn_kmsg_send_rdma(unsigned int to, void *lmsg,
						unsigned int msg_size, unsigned int rw_size)
{
    if (send_callback_rdma == NULL) {
		struct pcn_kmsg_hdr *hdr = lmsg;
		printk(KERN_ERR"%s: No send fn. from=%u, type=%d, msg_size=%u "
		"rw_size=%u\n", __func__, hdr->from_nid, hdr->type, msg_size, rw_size);
        return -ENOENT;
    }

    return send_callback_rdma(to, lmsg, msg_size, rw_size);
}

void pcn_kmsg_handle_remote_rdma_request(
								void *inc_lmsg, void *paddr)
{
	if (!memcmp(msg_layer,"IB", 2))
		handle_rdma_callback(inc_lmsg, paddr);
	else
		printk(KERN_ERR "%s: current msg_layer (%s) is not \"IB\"\n",
														__func__, msg_layer);
}


EXPORT_SYMBOL(pcn_kmsg_alloc_msg);
EXPORT_SYMBOL(pcn_kmsg_free_msg);
EXPORT_SYMBOL(pcn_kmsg_send_rdma);
EXPORT_SYMBOL(pcn_kmsg_send);
EXPORT_SYMBOL(pcn_kmsg_unregister_callback);
EXPORT_SYMBOL(pcn_kmsg_register_callback);
EXPORT_SYMBOL(pcn_kmsg_handle_remote_rdma_request);
