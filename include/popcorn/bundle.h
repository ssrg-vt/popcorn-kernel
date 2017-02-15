#ifndef __POPCORN_RACK_H__
#define __POPCORN_RACK_H__

#define MAX_POPCORN_NODES 32
#define MAX_BUNDLE_ID 32
#define MAX_KERNEL_IDS 2
//#define MAX_KERNEL_IDS NR_CPUS

enum popcorn_node_arch {
	POPCORN_NODE_X86 = 0,
	POPCORN_NODE_ARM = 1,
	POPCORN_NODE_POWERPC = 2,
	POPCORN_NODE_SPARC = 3,
	POPCORN_NODE_UNKNOWN,
};

struct popcorn_node;

static inline bool get_popcorn_node_arch(int nid) {
	return (nid % 2 == 0) ? POPCORN_NODE_X86 : POPCORN_NODE_ARM;
}

extern int my_nid;

bool is_popcorn_node_online(int nid);

int popcorn_nodes_init(void);
#endif
