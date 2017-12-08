#include <asm/bug.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/mm.h>

#include <popcorn/pcn_kmsg.h>
#include <popcorn/bundle.h>
#include <popcorn/debug.h>
#include "types.h"

struct popcorn_node {
	enum popcorn_arch arch;
	int bundle_id;

	bool is_connected;
};


static struct popcorn_node popcorn_nodes[MAX_POPCORN_NODES];

bool get_popcorn_node_online(int nid)
{
	return popcorn_nodes[nid].is_connected;
}
EXPORT_SYMBOL(get_popcorn_node_online);

void set_popcorn_node_online(int nid, bool online)
{
    popcorn_nodes[nid].is_connected = online;
}
EXPORT_SYMBOL(set_popcorn_node_online);


int my_nid __read_mostly = -1;
EXPORT_SYMBOL(my_nid);

const int my_arch =
#ifdef CONFIG_X86_64
	POPCORN_ARCH_X86;
#elif defined(CONFIG_ARM64)
	POPCORN_ARCH_ARM;
#elif defined(CONFIG_PPC64)
	POPCORN_ARCH_PPC;
#else
	POPCORN_ARCH_UNKNOWN;
#endif
EXPORT_SYMBOL(my_arch);

int get_popcorn_node_arch(int nid)
{
	return popcorn_nodes[nid].arch;
}
EXPORT_SYMBOL(get_popcorn_node_arch);

const char *archs_sz[] = {
	"aarch64",
	"x86_64",
	"ppc64le",
};


void notify_my_node_info(int nid)
{
	node_info_t info = {
		.header = {
			.type = PCN_KMSG_TYPE_NODE_INFO,
			.prio = PCN_KMSG_PRIO_NORMAL,
		},
		.nid = my_nid,
		.arch = my_arch,
	};

	pcn_kmsg_send(nid, &info, sizeof(info));	
}
EXPORT_SYMBOL(notify_my_node_info);

static bool my_node_info_printed = false;

static int handle_node_info(struct pcn_kmsg_message *msg)
{
	node_info_t *info = (node_info_t *)msg;

	if (my_nid != -1 && !my_node_info_printed) {
		popcorn_nodes[my_nid].arch = my_arch;
		my_node_info_printed = true;
	}

	PCNPRINTK("   %d joined, %s\n", info->nid, archs_sz[info->arch]);
	popcorn_nodes[info->nid].arch = info->arch;
	smp_mb();

	pcn_kmsg_free_msg(msg);
	return 0;
}

int __init popcorn_nodes_init(void)
{
	int i;
	BUG_ON(my_arch == POPCORN_ARCH_UNKNOWN);

	for (i = 0; i < MAX_POPCORN_NODES; i++) {
		struct popcorn_node *pn = popcorn_nodes + i;

		pn->is_connected = false;
		pn->arch = POPCORN_ARCH_UNKNOWN;
		pn->bundle_id = -1;
	}

	REGISTER_KMSG_HANDLER(PCN_KMSG_TYPE_NODE_INFO, node_info);

	return 0;
}
