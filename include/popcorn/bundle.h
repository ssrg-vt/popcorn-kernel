// SPDX-License-Identifier: GPL-2.0, 3-clause BSD
#ifndef __POPCORN_RACK_H__
#define __POPCORN_RACK_H__

#define MAX_POPCORN_NODES 32
#if (MAX_POPCORN_NODES > 62)
#error Currently support up to 62 nodes
#endif

enum popcorn_arch {
	POPCORN_ARCH_UNKNOWN = -1,
	POPCORN_ARCH_X86 = 1,
	POPCORN_ARCH_PPC = 2,
	POPCORN_ARCH_MAX,
};

extern int my_nid;
extern const enum popcorn_arch my_arch;

bool get_popcorn_node_online(int nid);
int get_popcorn_node_arch(int nid);
int popcorn_nodes_init(void);
void set_popcorn_node_online(int nid, bool online);
void broadcast_my_node_info(int nr_nodes);

struct popcorn_thread_status {
	int current_nid;
	int peer_nid;
	pid_t peer_pid;
};

struct popcorn_node_info {
	unsigned int status;
	int arch;
	int distance;
};

#endif
