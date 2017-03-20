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
	enum popcorn_node_arch arch;

	bool is_connected;
};

struct popcorn_node this_node = {
};


static struct popcorn_node popcorn_nodes[MAX_POPCORN_NODES];

bool is_popcorn_node_online(int nid)
{
	return popcorn_nodes[nid].is_connected;
}
EXPORT_SYMBOL(is_popcorn_node_online);

void set_popcorn_node_online(int nid)
{
    popcorn_nodes[nid].is_connected = true;
}
EXPORT_SYMBOL(set_popcorn_node_online);

void set_popcorn_node_offline(int nid)
{
    popcorn_nodes[nid].is_connected = false;
}
EXPORT_SYMBOL(set_popcorn_node_offline);

int my_nid __read_mostly = -1;
EXPORT_SYMBOL(my_nid);

const int my_arch =
#ifdef CONFIG_X86_64
	POPCORN_NODE_X86;
#elif defined(CONFIG_ARM64)
	POPCORN_NODE_ARM;
#elif defined(CONFIG_PPC64)
	POPCORN_NODE_PPC;
#elif defined(CONFIG_SPARC64)
	POPCORN_NODE_SPARC;
#else
	POPCORN_NODE_UNKNOWN;
#endif
EXPORT_SYMBOL(my_arch);


int __init popcorn_nodes_init(void)
{
	int i;
	BUG_ON(my_arch == POPCORN_NODE_UNKNOWN);

	for (i = 0; i < MAX_POPCORN_NODES; i++) {
		struct popcorn_node *pn = popcorn_nodes + i;

		pn->is_connected = false;
		pn->arch = POPCORN_NODE_UNKNOWN;
	}

	return 0;
}
