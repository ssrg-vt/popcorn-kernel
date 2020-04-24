#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/slab.h>
#include <asm/uaccess.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/percpu.h>

#include <popcorn/stat.h>
#include "types.h"

extern int max_ongoing_pf_req_per_thread;
#define MAX_STR_LEN 20
#define BUFSIZE 4*4096

static int __show_stats(struct seq_file *seq, void *v)
{
	return 0;
}

static ssize_t __write_stats(struct file *file, const char __user *buffer, size_t size, loff_t *offset)
{
	char tmp[MAX_STR_LEN];

	memset(tmp, 0, MAX_STR_LEN);
	if (copy_from_user(tmp, buffer, MAX_STR_LEN))
		return -EFAULT;

	// don't call it durring runtime no lock protecting this
	max_ongoing_pf_req_per_thread = 
			simple_strtol(tmp, NULL, 10); //dec
	//printk("size(len) %lu, num %d\n",
	//		size, max_ongoing_pf_req_per_thread);
	return size;
}

static ssize_t __seq_read(struct file *filp, char *buf,
							size_t count, loff_t *offset)
{
	int len = 0;
	char *tmp = kzalloc(BUFSIZE, GFP_KERNEL);

	if (*offset > 0)
		return 0;
	
   	len += snprintf(tmp + strlen(tmp), BUFSIZE, "%d", max_ongoing_pf_req_per_thread);
    
	if( len > BUFSIZE )
        len = BUFSIZE;

    if (copy_to_user(buf, tmp, len))
		return -EFAULT;
	*offset += len;

    //printk("count %lu len %d *offset %llu\n", count, len, *offset);
    kfree(tmp);
	return len;
}

static int __open_stats(struct inode *inode, struct file *file)
{
	return single_open(file, __show_stats, inode->i_private);
}

static struct file_operations stats_ops = {
	.owner = THIS_MODULE,
	.open = __open_stats,
	.read = __seq_read,
	.llseek  = seq_lseek,
	.release = single_release,
	.write = __write_stats,
};

static struct proc_dir_entry *proc_entry = NULL;

int prefetch_usr_interface_init(void)
{
	proc_entry = proc_create("popcorn_pf_interface", S_IRUGO | S_IWUGO, NULL, &stats_ops);
	if (proc_entry == NULL) {
		printk(KERN_ERR"cannot create proc_fs entry for popcorn prefetch interface\n");
		return -ENOMEM;
	}
	return 0;
}
