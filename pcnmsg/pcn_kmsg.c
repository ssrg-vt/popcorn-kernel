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

pcn_kmsg_cbftn callbacks[PCN_KMSG_TYPE_MAX];
EXPORT_SYMBOL(callbacks);

send_cbftn send_callback;
EXPORT_SYMBOL(send_callback);

/* Initialize callback table to null, set up control and data channels */
int __init pcn_kmsg_init(void)
{
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

int pcn_kmsg_send_long(unsigned int to, void *lmsg, unsigned int size)
{
	if (send_callback == NULL) {
		struct pcn_kmsg_hdr *hdr = (struct pcn_kmsg_hdr *)lmsg;

		printk(KERN_ERR"%s: No send fn. from=%u, type=%d, size=%u\n",
					__func__, hdr->from_nid, hdr->type, size);
		// msleep(100);
		//printk("Waiting for call back function to be registered\n");
		return -ENOENT;
	}

	return send_callback(to, (struct pcn_kmsg_message *)lmsg, size);
}

int pcn_kmsg_send(unsigned int to, void *msg)
{
	return pcn_kmsg_send_long(to, msg, sizeof(struct pcn_kmsg_message));
}

void *pcn_kmsg_alloc_msg(size_t size)
{
	return vmalloc(size);
}

void pcn_kmsg_free_msg(void *msg)
{
	vfree(msg);
}

EXPORT_SYMBOL(pcn_kmsg_alloc_msg);
EXPORT_SYMBOL(pcn_kmsg_free_msg);
EXPORT_SYMBOL(pcn_kmsg_send_long);
EXPORT_SYMBOL(pcn_kmsg_send);
EXPORT_SYMBOL(pcn_kmsg_unregister_callback);
EXPORT_SYMBOL(pcn_kmsg_register_callback);
