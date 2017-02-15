#include <asm/bug.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/mm.h>

#include <popcorn/bundle.h>
#include "types.h"

struct popcorn_node {
	unsigned int id;
	unsigned int subid;
	enum popcorn_node_arch arch;

	bool is_connected;
};

struct popcorn_node popcorn_node = {
	.id = -1,
	.subid = -1,
#ifdef CONFIG_X86
	.arch = POPCORN_NODE_X86,
#elif defined(CONFIG_ARM) || defined(CONFIG_ARM64)
	.arch = POPCORN_NODE_ARM,
#else
	.arch = POPCORN_NODE_UNKNOWN,
#endif
};

const unsigned long __node_addrs[] = {
	0x000000ul,
	0x000001ul,
	0x000002ul,
};

static struct popcorn_node popcorn_nodes[MAX_POPCORN_NODES];

bool is_popcorn_node_online(int nid)
{
	return true;
	//return popcorn_nodes[nid].is_connected;
}

int my_nid __read_mostly = -1;
EXPORT_SYMBOL(my_nid);

static int __connect_to_popcorn_nodes(void)
{
	// TODO: connect to other node. Now deal with loopback only
	popcorn_nodes[0].is_connected = true;
	popcorn_nodes[0].arch = POPCORN_NODE_X86;

	return 0;
}


static int __init parse_popcorn_node_opt(char *str)
{
	unsigned int ids[3] = {-1};
	get_options(str, sizeof(ids) / sizeof(unsigned int), ids);

	popcorn_node.id = my_nid = ids[1];
	popcorn_node.subid = ids[2];

	printk(KERN_INFO"Popcorn node: id=%d,%d arch=%s\n",
			popcorn_node.id, popcorn_node.subid, 
			popcorn_node.arch == POPCORN_NODE_X86 ? "x86" :
			popcorn_node.arch == POPCORN_NODE_ARM ? "arm" : "???" );

	return 0;
}
early_param("popcorn_node", parse_popcorn_node_opt);


int __init popcorn_nodes_init(void)
{
	int i;
	BUG_ON(popcorn_node.arch == POPCORN_NODE_UNKNOWN);

	if (popcorn_node.id < 0 || popcorn_node.id >= MAX_POPCORN_NODES) {
		printk(KERN_ERR"********************************************");
		printk(KERN_ERR"***                                      ***");
		printk(KERN_ERR"***                                      ***");
		printk(KERN_ERR"***    Set the popcorn_node in cmdline   ***");
		printk(KERN_ERR"***                                      ***");
		printk(KERN_ERR"***                                      ***");
		printk(KERN_ERR"********************************************");
		BUG();
	}

	for (i = 0; i < MAX_POPCORN_NODES; i++) {
		struct popcorn_node *pn = popcorn_nodes + i;

		pn->is_connected = false;
		pn->arch = POPCORN_NODE_UNKNOWN;
	}

	__connect_to_popcorn_nodes();

	return 0;
}
