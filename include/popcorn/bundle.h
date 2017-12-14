#ifndef __POPCORN_RACK_H__
#define __POPCORN_RACK_H__

#define MAX_POPCORN_NODES 32
#if (MAX_POPCORN_NODES > 62)
#error Currently support up to 62 nodes
#endif

enum popcorn_arch {
	POPCORN_ARCH_ARM = 0,
	POPCORN_ARCH_X86 = 1,
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

struct popcorn_thread_status {
	int current_nid;
	int proposed_nid;
	int peer_nid;
	pid_t peer_pid;
};

struct popcorn_node_info {
	unsigned int status;
	int arch;
	int distance;
};

#endif
