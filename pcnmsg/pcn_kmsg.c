/*
 * pcn_kmesg.c - Kernel Module for Popcorn Messaging Layer over Socket
 */

#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/vmalloc.h>

#include <linux/pcn_kmsg.h>

pcn_kmsg_cbftn callbacks[PCN_KMSG_TYPE_MAX];
EXPORT_SYMBOL(callbacks);

send_cbftn send_callback;
EXPORT_SYMBOL(send_callback);

unsigned int my_cpu = 0;
enum my_cpu_enum {
	MY_CPU_ARM,
	MY_CPU_X86,
	MY_CPU_UNKNOWN,
};

/* Initialize callback table to null, set up control and data channels */
int __init pcn_kmsg_init(void)
{
	send_callback = NULL;

#if defined(CONFIG_ARM64)
	my_cpu = MY_CPU_ARM;
#elif defined(CONFIG_X86_64)
	my_cpu = MY_CPU_X86;
#else
	my_cpu = MY_CPU_UNKNOWN;
	printk(KERN_ERR"%s: unkown architecture detected\n", __func__);
	return -EINVAL;
#endif

	printk("%s: done\n", __func__);
	return 0;
}

int pcn_kmsg_register_callback(enum pcn_kmsg_type type, pcn_kmsg_cbftn callback)
{
	if (type >= PCN_KMSG_TYPE_MAX)
		return -ENODEV; /* invalid type */

	printk("%s: registering %d \n",__func__, type);
	callbacks[type] = callback;
	return 0;
}

int pcn_kmsg_unregister_callback(enum pcn_kmsg_type type)
{
	if (type >= PCN_KMSG_TYPE_MAX)
		return -ENODEV;

	printk("Unregistering callback %d\n", type);
	callbacks[type] = NULL;
	return 0;
}

int pcn_kmsg_send(unsigned int dest_cpu, struct pcn_kmsg_message *msg)
{
	return pcn_kmsg_send_long(dest_cpu, (struct pcn_kmsg_long_message *)msg,
				  sizeof(struct pcn_kmsg_message)-sizeof(struct pcn_kmsg_hdr));
}

int pcn_kmsg_send_long(unsigned int dest_cpu, struct pcn_kmsg_long_message *lmsg, unsigned int payload_size)
{
	int ret = 0;

	if (send_callback == NULL) {
		msleep(100);
		//printk("Waiting for call back function to be registered\n");
		return 0;
	}

	ret = send_callback(dest_cpu, (struct pcn_kmsg_message *)lmsg, payload_size);

	return ret;
}

void pcn_kmsg_free_msg(void *msg)
{
	vfree(msg);
}

/* TODO */
inline int pcn_kmsg_get_node_ids(uint16_t *nodes, int len, uint16_t *self)
{
#if defined(CONFIG_ARM64)
	*self = 0;
#elif defined(CONFIG_X86_64)
	*self = 1;
#else
	printk(" Unkown architecture detected\n");
	*self = 0;
#endif

	return 0;
}

EXPORT_SYMBOL(pcn_kmsg_free_msg);
EXPORT_SYMBOL(pcn_kmsg_send_long);
EXPORT_SYMBOL(pcn_kmsg_send);
EXPORT_SYMBOL(pcn_kmsg_unregister_callback);
EXPORT_SYMBOL(pcn_kmsg_register_callback);
EXPORT_SYMBOL(pcn_kmsg_get_node_ids);
