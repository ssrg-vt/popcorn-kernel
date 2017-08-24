#ifndef __POPCORN_RACK_H__
#define __POPCORN_RACK_H__

#define MAX_POPCORN_NODES 32
#define MAX_BUNDLE_ID 32

enum popcorn_arch {
	POPCORN_ARCH_X86 = 0,
	POPCORN_ARCH_ARM = 1,
	POPCORN_ARCH_PPC = 2,
	POPCORN_ARCH_UNKNOWN,
};

extern int my_nid;
extern const int my_arch;

bool get_popcorn_node_online(int nid);
void set_popcorn_node_online(int nid, bool online);

int get_popcorn_node_arch(int nid);

void notify_my_node_info(int nid);

int popcorn_nodes_init(void);
#endif
