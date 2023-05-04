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
const int origin_nid = 0;
const int remote_nid = 1;

EXPORT_SYMBOL(my_nid);

const enum popcorn_arch my_arch =
#ifdef CONFIG_X86_64
	POPCORN_ARCH_X86;
#elif defined(CONFIG_ARM64)
	POPCORN_ARCH_ARM;
#elif defined(CONFIG_PPC64)
	POPCORN_ARCH_PPC;
#elif defined (CONFIG_RISCV)
	POPCORN_ARCH_RISCV;
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
	"riscv64",
};

#define TRANSFER_WITH_PCIE_AXI \
		pcn_kmsg_has_features(PCN_KMSG_FEATURE_PCIE_AXI)

void broadcast_my_node_info(int nr_nodes)
{
	int i;
	node_info_t info = {
		.nid = my_nid,
		.arch = my_arch,
	};

	if(TRANSFER_WITH_PCIE_AXI){
		if(!my_nid)	{
			PCNPRINTK("This is the origin node\n");
			return;
		}
		else {
			pcn_kmsg_send(PCN_KMSG_TYPE_NODE_INFO, origin_nid, &info, sizeof(info));
			return;
		}
	}
	else {
		   for (i = 0; i < nr_nodes; i++) {
			 if (i == my_nid) continue;
			 pcn_kmsg_send(PCN_KMSG_TYPE_NODE_INFO, i, &info, sizeof(info));
		}
	}
	
}
EXPORT_SYMBOL(broadcast_my_node_info);

static bool my_node_info_printed = false;

static void process_node_info(struct work_struct *work)
{
	//node_info_t *info = (node_info_t *)msg;
	START_KMSG_WORK(node_info_t, info, work);

	if (my_nid != -1 && !my_node_info_printed) {
		popcorn_nodes[my_nid].arch = my_arch;
		my_node_info_printed = true;
	}

	PCNPRINTK("   %d joined, %s\n", info->nid, archs_sz[info->arch]);
	popcorn_nodes[info->nid].arch = info->arch;
	
	smp_mb();
	END_KMSG_WORK(info);

	if(TRANSFER_WITH_PCIE_AXI){
		set_popcorn_node_online(info->nid, "true");
		if(my_nid == origin_nid) {
			node_info_t org_info = {
				.nid = my_nid,
				.arch = my_arch,
			};
			pcn_kmsg_send(PCN_KMSG_TYPE_NODE_INFO, remote_nid, &org_info, sizeof(org_info));
		}
		else {
			PCNPRINTK("This is the remote node\n");
		}
	}

}

DEFINE_KMSG_WQ_HANDLER(node_info);

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

	REGISTER_KMSG_WQ_HANDLER(PCN_KMSG_TYPE_NODE_INFO, node_info);

	return 0;
}
