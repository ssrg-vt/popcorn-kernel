#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/vmalloc.h>

#include <popcorn/pcn_kmsg.h>

#define MSG_LENGTH 1024

struct test_msg_t
{
	struct pcn_kmsg_hdr header;
	unsigned char payload[MSG_LENGTH];
};

static int handle_self_test(struct pcn_kmsg_message* inc_msg)
{
	printk(KERN_INFO "%s: message handler is called from cpu %d successfully.\n",
		__func__, inc_msg->header.from_cpu);

	printk(KERN_INFO "%s: %s\n", __func__, inc_msg->payload);

	return 0;
}

static int __init msg_test_init(void)
{
	struct test_msg_t *msg;
	int payload_size = MSG_LENGTH;

	printk(KERN_INFO "%s: Message Layer Test Module loaded\n", __func__);

	pcn_kmsg_register_callback(PCN_KMSG_TYPE_SELFIE_TEST, handle_self_test);

	msg = (struct test_msg_t *) vmalloc(sizeof(struct test_msg_t));
	msg->header.type= PCN_KMSG_TYPE_SELFIE_TEST;
	memset(msg->payload, 'b', payload_size);

	/* dst_cpu 0: Target node id is 8 */
	pcn_kmsg_send_long(0, (struct pcn_kmsg_long_message*)msg, payload_size);

	return 0;
}

static void __exit msg_test_exit(void)
{
	printk(KERN_INFO "%s: Message Layer Test Module exiting\n", __func__);
}

module_init(msg_test_init);
module_exit(msg_test_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Jingoo Han <jingoo@vt.edu>");
