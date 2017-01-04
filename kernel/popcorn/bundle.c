#include <linux/kernel.h>
#include <asm/bug.h>
#include <linux/errno.h>
#include <linux/init.h>

#include <popcorn/bundle.h>

struct bundle bundle = {
	.id = -1,
	.subid = -1,
#ifdef CONFIG_X86
	.type = BUNDLE_NODE_X86,
#elif defined(CONFIG_ARM) || defined(CONFIG_ARM64)
	.type = BUNDLE_NODE_ARM,
#else
	.type = BUNDLE_NODE_UNKNOWN,
#endif
	.bundles_online = {0},
};

const unsigned long __node_addrs[] = {
	0x000000ul,
	0x000001ul,
	0x000002ul,
};

static int __init parse_bundle_id_opt(char *str)
{
	unsigned int ids[3] = {-1};
	get_options(str, sizeof(ids) / sizeof(unsigned int), ids);

	bundle.id = ids[1];
	bundle.subid = ids[2];

	printk(KERN_INFO"Popcorn bundle: id=%d,%d type=%s\n",
			bundle.id, bundle.subid, 
			bundle.type == BUNDLE_NODE_X86 ? "x86" :
			bundle.type == BUNDLE_NODE_ARM ? "arm" : "???" );

	return 0;
}
early_param("bundle_id", parse_bundle_id_opt);


static bool __init __connect_to(unsigned long bid)
{
	/* TODO connect to peer bundles */
	return true;
}

bool is_bundle_online(unsigned int bid)
{
	return test_bit(bid, bundle.bundles_online);
}

static int __init __connect_to_other_bundles(void)
{
	int i;
	int connected = 0;

	for (i = 0; i < bundle.id; i++) {
		if (__connect_to(i)) {
			set_bit(i, bundle.bundles_online);
			connected++;
		}
	}
	printk(KERN_INFO"Connected to bundles at %*pbl\n",
			MAX_BUNDLE_ID, bundle.bundles_online);

	return connected;
}

int __init setup_bundle_node(void)
{
	BUG_ON(bundle.type == BUNDLE_NODE_UNKNOWN);

	if (bundle.id < 0 || bundle.id >= MAX_BUNDLE_ID) {
		printk(KERN_ERR"****** Set the bundle_id in cmdline *****");
		BUG();
	}

	set_bit(bundle.id, bundle.bundles_online);

	__connect_to_other_bundles();

	return 0;
}
