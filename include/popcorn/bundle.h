#ifndef __POPCORN_RACK_H__
#define __POPCORN_RACK_H__

#include <linux/process_server.h>

#define MAX_POPCORN_NODES 32
#define MAX_BUNDLE_ID 32

enum popcorn_node_arch {
	POPCORN_NODE_X86 = 0,
	POPCORN_NODE_ARM = 1,
	POPCORN_NODE_UNKNOWN = 2,
};

struct popcorn_node;

static inline bool get_popcorn_node_arch(int nid) {
	return (nid % 2 == 0) ? POPCORN_NODE_X86 : POPCORN_NODE_ARM;
}

int get_nid(void);

void add_memory_entry_in_out(memory_t *m, int nid, bool in);
#define add_memory_entry_in(m, nid)	\
	add_memory_entry_in_out((m), (nid), true)
#define add_memory_entry_out(m, nid)	\
	add_memory_entry_in_out((m), (nid), false)

memory_t *find_memory_entry_in_out(int nid, int pid, bool in);
#define find_memory_entry_in(nid, pid) \
	find_memory_entry_in_out((nid), (pid), true)
#define find_memory_entry_out(nid, pid) \
	find_memory_entry_in_out((nid), (pid), false)


bool is_popcorn_node_online(int nid);

int popcorn_nodes_init(void);
#endif
