#ifndef __POPCORN_RACK_H__
#define __POPCORN_RACK_H__

#define MAX_POPCORN_NODES 32
#define MAX_BUNDLE_ID 32
#define MAX_KERNEL_IDS 2

enum popcorn_node_arch {
	POPCORN_NODE_X86 = 0,
	POPCORN_NODE_ARM = 1,
	POPCORN_NODE_PPC = 2,
	POPCORN_NODE_SPARC = 3,
	POPCORN_NODE_UNKNOWN,
};


extern int my_nid;
extern const int my_arch;


bool is_popcorn_node_online(int nid);
void set_popcorn_node_online(int nid);
void set_popcorn_node_offline(int nid);

int popcorn_nodes_init(void);
#endif
