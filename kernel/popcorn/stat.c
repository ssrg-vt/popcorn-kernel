#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/slab.h>
#include <asm/uaccess.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/percpu.h>

#include <popcorn/pcn_kmsg.h>
#include <popcorn/stat.h>

static unsigned long long sent_stats[PCN_KMSG_TYPE_MAX] = {0};
static unsigned long long recv_stats[PCN_KMSG_TYPE_MAX] = {0};

static DEFINE_PER_CPU(unsigned long long, bytes_sent) = 0;
static DEFINE_PER_CPU(unsigned long long, bytes_recv) = 0;
static DEFINE_PER_CPU(unsigned long long, bytes_rdma_written) = 0;
static DEFINE_PER_CPU(unsigned long long, bytes_rdma_read) = 0;

const char *pcn_kmsg_type_name[PCN_KMSG_TYPE_MAX] = {
	[PCN_KMSG_TYPE_TASK_MIGRATE] = "migration",
	[PCN_KMSG_TYPE_VMA_INFO_REQUEST] = "VMA info",
	[PCN_KMSG_TYPE_VMA_OP_RESPONSE] = "VMA op",
	[PCN_KMSG_TYPE_REMOTE_PAGE_REQUEST] = "remote page",
	[PCN_KMSG_TYPE_PAGE_INVALIDATE_REQUEST] = "invalidate",
	[PCN_KMSG_TYPE_FUTEX_REQUEST] = "futex",
};

void account_pcn_message_sent(struct pcn_kmsg_message *msg)
{
	struct pcn_kmsg_hdr *h = (struct pcn_kmsg_hdr *)msg;
	this_cpu_add(bytes_sent, h->size);
#ifdef CONFIG_POPCORN_STAT
	sent_stats[h->type]++;
#endif
}

void account_pcn_message_recv(struct pcn_kmsg_message *msg)
{
	struct pcn_kmsg_hdr *h = (struct pcn_kmsg_hdr *)msg;
	this_cpu_add(bytes_recv, h->size);
#ifdef CONFIG_POPCORN_STAT
	recv_stats[h->type]++;
#endif
}

void account_pcn_rdma_write(size_t size)
{
	this_cpu_add(bytes_rdma_written, size);
}

void account_pcn_rdma_read(size_t size)
{
	this_cpu_add(bytes_rdma_read, size);
}

void fh_action_stat(struct seq_file *seq, void *);

static int __show_stats(struct seq_file *seq, void *v)
{
	int i;
	unsigned long long sent = 0;
	unsigned long long recv = 0;

	for_each_possible_cpu(i) {
		sent += per_cpu(bytes_sent, i);
		recv += per_cpu(bytes_recv, i);
	}
	seq_printf(seq, POPCORN_STAT_FMT, sent, recv, "total network I/O");

	recv = sent = 0;
	for_each_possible_cpu(i) {
		sent += per_cpu(bytes_rdma_written, i);
		recv += per_cpu(bytes_rdma_read, i);
	}
	seq_printf(seq, POPCORN_STAT_FMT, sent, recv, "RDMA");

	pcn_kmsg_stat(seq, v);

	seq_printf(seq, "-----------------------------------------------\n");
	for (i = PCN_KMSG_TYPE_STAT_START + 1; i < PCN_KMSG_TYPE_STAT_END; i++) {
		seq_printf(seq, POPCORN_STAT_FMT,
				sent_stats[i], recv_stats[i], pcn_kmsg_type_name[i] ? : "");
	}
	seq_printf(seq, "-----------------------------------------------\n");

	fh_action_stat(seq, v);
	return 0;
}

static int __open_stats(struct inode *inode, struct file *file)
{
	return single_open(file, __show_stats, inode->i_private);
}

static struct file_operations stats_ops = {
	.owner = THIS_MODULE,
	.open = __open_stats,
	.read = seq_read,
	.llseek  = seq_lseek,
	.release = single_release,
};

static struct proc_dir_entry *proc_entry = NULL;

int statistics_init(void)
{
	proc_entry = proc_create("popcorn_stat", S_IRUGO, NULL, &stats_ops);
	if (proc_entry == NULL) {
		printk(KERN_ERR"cannot create proc_fs entry for popcorn stats\n");
		return -ENOMEM;
	}
	return 0;
}
