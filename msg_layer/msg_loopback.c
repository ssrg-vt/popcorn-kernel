#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>

#include "common.h"

static int loopback_kmsg_send_long(unsigned int nid, struct pcn_kmsg_long_message *lmsg, unsigned int size)
{
	void *msg;
	struct pcn_kmsg_hdr *hdr;
	pcn_kmsg_cbftn fn;

	msg = pcn_kmsg_alloc_msg(size);
	BUG_ON(!msg);
	memcpy(msg, lmsg, size);

	hdr = (struct pcn_kmsg_hdr *)msg;
	hdr->from_nid = my_nid;
	hdr->size = size;

	fn = callbacks[hdr->type];
	if (!fn) {
		printk(KERN_ERR"%s: NULL FN", __func__);
		pcn_kmsg_free_msg(msg);
		return -ENOENT;
	}
	// printk(KERN_ERR"%s: CALL %d %d\n", __func__, hdr->type, hdr->size);

	fn(msg);
	return 0;
}

static int __init loopback_load(void)
{
	MSGPRINTK(KERN_INFO"Popcorn message layer loopback loaded\n");

	my_nid = 0;
	set_popcorn_node_online(my_nid);

	send_callback = (send_cbftn)loopback_kmsg_send_long;

	return 0;
}

static void loopback_unload(void)
{
	send_callback = NULL;
	MSGPRINTK(KERN_INFO"Popcorn message layer loopback unloaded\n");
}

module_init(loopback_load);
module_exit(loopback_unload);
MODULE_LICENSE("GPL");
