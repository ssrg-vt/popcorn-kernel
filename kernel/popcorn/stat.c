#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <asm/uaccess.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
//#include <linux/stat.h>

#include <popcorn/pcn_kmsg.h>
#include <popcorn/stat.h>

static unsigned long sent_stats[PCN_KMSG_TYPE_MAX] = {0};
static unsigned long recv_stats[PCN_KMSG_TYPE_MAX] = {0};

static unsigned long long bytes_sent = 0;
static unsigned long long bytes_recv = 0;

void account_pcn_message_sent(struct pcn_kmsg_message *msg)
{
	struct pcn_kmsg_hdr *h = (struct pcn_kmsg_hdr *)msg;
	bytes_sent += h->size;
#ifdef CONFIG_POPCORN_STAT
	sent_stats[h->type]++;
#endif
}

void account_pcn_message_recv(struct pcn_kmsg_message *msg)
{
	struct pcn_kmsg_hdr *h = (struct pcn_kmsg_hdr *)msg;
	bytes_recv += h->size;
#ifdef CONFIG_POPCORN_STAT
	recv_stats[h->type]++;
#endif
}


#define PROC_BUF_SIZE 8192
static ssize_t __read_stats(struct file *filp, char *usr_buf, size_t count, loff_t *offset)
{
	int i;
	char *buf;
	int len = 0;
	unsigned long *stats;

	buf = kzalloc(PROC_BUF_SIZE, GFP_KERNEL);
	if (!buf)
		BUG();

	if (*offset == 0) {
		stats = sent_stats;
		len += snprintf(buf, PROC_BUF_SIZE, "%llu ", bytes_sent);
	} else if (*offset == 1) {
		stats = recv_stats;
		len += snprintf(buf, PROC_BUF_SIZE, "%llu ", bytes_recv);
	} else {
		return 0;
	}

	len += pcn_kmsg_stat(buf + len, PROC_BUF_SIZE - len);

	for (i = PCN_KMSG_TYPE_STAT_START + 1; i < PCN_KMSG_TYPE_STAT_END; i++) {
		len += snprintf(buf + len, PROC_BUF_SIZE - len,
						"%lu ", stats[i]);
		if (len >= PROC_BUF_SIZE) {
			len = PROC_BUF_SIZE;
			printk(KERN_WARNING "Dropping logs \n");
			break;
		}
	}
	len += snprintf(buf + len, PROC_BUF_SIZE - len, "\n");

	if (copy_to_user(usr_buf, buf, len)) {
		kfree(buf);
		return -EFAULT;
	}

	kfree(buf);
	*offset = *offset + 1;
	return len;
}

static int __show_stats(struct seq_file *seq, void *v)
{
	return 0;
}

static int __open_stats(struct inode *inode, struct file *file)
{
	return single_open(file, __show_stats, inode->i_private);
}

static struct file_operations stats_ops = {
	.owner = THIS_MODULE,
	.open = __open_stats,
	.read = __read_stats,
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
