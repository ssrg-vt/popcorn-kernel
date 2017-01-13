#include <asm/bug.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/list.h>
#include <linux/spinlock.h>

#include <linux/process_server.h>
#include <popcorn/bundle.h>

struct popcorn_node {
	unsigned int id;
	unsigned int subid;
	enum popcorn_node_arch arch;

	bool is_connected;
	struct list_head memory[2];
	spinlock_t memory_lock[2];
};

struct popcorn_node popcorn_node = {
	.id = -1,
	.subid = -1,
#ifdef CONFIG_X86
	.arch = POPCORN_NODE_X86,
#elif defined(CONFIG_ARM) || defined(CONFIG_ARM64)
	.arch = POPCORN_NODE_ARM,
#else
	.arch = POPCORN_NODE_UNKNOWN,
#endif
};

const unsigned long __node_addrs[] = {
	0x000000ul,
	0x000001ul,
	0x000002ul,
};

static struct popcorn_node popcorn_nodes[MAX_POPCORN_NODES];


void add_memory_entry_in_out(memory_t *m, int nid, bool in)
{
	unsigned long flags;

	spin_lock_irqsave(popcorn_nodes[nid].memory_lock + in, flags);
	list_add(&m->list, popcorn_nodes[nid].memory + in);
	spin_unlock_irqrestore(popcorn_nodes[nid].memory_lock + in, flags);
}

memory_t *find_memory_entry_in_out(int nid, int pid, bool in)
{
	memory_t *m = NULL;
	memory_t *found = NULL;
	unsigned long flags;
	struct list_head *list = popcorn_nodes[nid].memory + in;
	spinlock_t *lock = popcorn_nodes[nid].memory_lock + in;

	spin_lock_irqsave(lock, flags);
	list_for_each_entry(m, list, list) {
		BUG_ON(m->tgroup_home_cpu != nid);
		if (m->tgroup_home_id == pid) {
#ifdef CHECK_FOR_DUPLICATES
			if (found) {
				printk(KERN_ERR"%s: duplicates in list %s %s (cpu %d id %d)\n",
						__func__, found->path, m->path, nid, pid);
			}
			found = m;
#else
			found = m;
			break;
#endif
		}
	}
	spin_unlock_irqrestore(lock, flags);

	return found;
}


bool is_popcorn_node_online(int nid)
{
	return popcorn_nodes[nid].is_connected;
}

int get_nid(void)
{
	return popcorn_node.id;
}
EXPORT_SYMBOL(get_nid);

static int __connect_to_popcorn_nodes(void)
{
	// TODO: connect to other node. Now deal with loopback only
	popcorn_nodes[0].is_connected = true;
	popcorn_nodes[0].arch = POPCORN_NODE_X86;

	return 0;
}


static int __init parse_popcorn_node_opt(char *str)
{
	unsigned int ids[3] = {-1};
	get_options(str, sizeof(ids) / sizeof(unsigned int), ids);

	popcorn_node.id = ids[1];
	popcorn_node.subid = ids[2];

	printk(KERN_INFO"Popcorn node: id=%d,%d arch=%s\n",
			popcorn_node.id, popcorn_node.subid, 
			popcorn_node.arch == POPCORN_NODE_X86 ? "x86" :
			popcorn_node.arch == POPCORN_NODE_ARM ? "arm" : "???" );

	return 0;
}
early_param("popcorn_node", parse_popcorn_node_opt);


int __init popcorn_nodes_init(void)
{
	int i;
	BUG_ON(popcorn_node.arch == POPCORN_NODE_UNKNOWN);

	if (popcorn_node.id < 0 || popcorn_node.id >= MAX_POPCORN_NODES) {
		printk(KERN_ERR"********************************************");
		printk(KERN_ERR"***                                      ***");
		printk(KERN_ERR"***                                      ***");
		printk(KERN_ERR"***    Set the popcorn_node in cmdline   ***");
		printk(KERN_ERR"***                                      ***");
		printk(KERN_ERR"***                                      ***");
		printk(KERN_ERR"********************************************");
		BUG();
	}

	for (i = 0; i < MAX_POPCORN_NODES; i++) {
		struct popcorn_node *pn = popcorn_nodes + i;

		INIT_LIST_HEAD(pn->memory + 0);
		INIT_LIST_HEAD(pn->memory + 1);

		spin_lock_init(pn->memory_lock + 0);
		spin_lock_init(pn->memory_lock + 1);

		pn->is_connected = false;
		pn->arch = POPCORN_NODE_UNKNOWN;
	}

	__connect_to_popcorn_nodes();

	return 0;
}
