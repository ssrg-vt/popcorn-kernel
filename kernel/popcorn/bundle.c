// SPDX-License-Identifier: GPL-2.0, BSD
/*
 * /kernel/popcorn/bundle.c
 *
 * Popcorn node init
 *
 * Original file developed by SSRG at Virginia Tech.
 *
 * author, Javier Malave, Rebecca Shapiro, Andrew Hughes,
 * Narf Industries 2020 (modifications for upstream RFC)
 */

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

const enum popcorn_arch my_arch = POPCORN_ARCH_X86;
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

void broadcast_my_node_info(int nr_nodes)
{
	int i;
	node_info_t info = {
		.nid = my_nid,
		.arch = my_arch,
	};
	for (i = 0; i < nr_nodes; i++) {
		if (i == my_nid)
                        continue;
		pcn_kmsg_send(PCN_KMSG_TYPE_NODE_INFO, i, &info, sizeof(info));
	}
}
EXPORT_SYMBOL(broadcast_my_node_info);

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

	pcn_kmsg_done(msg);
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
