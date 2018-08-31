#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/sched/task_stack.h>
#include <popcorn/bundle.h>

void print_page_data(unsigned char *addr)
{
	int i;
	for (i = 0; i < PAGE_SIZE; i++) {
		if (i % 16 == 0) {
			printk(KERN_INFO"%08lx:", (unsigned long)(addr + i));
		}
		if (i % 4 == 0) {
			printk(" ");
		}
		printk("%02x", *(addr + i));
	}
	printk("\n");
}

void print_page_signature(unsigned char *addr)
{
	unsigned char *p = addr;
	int i, j;
	for (i = 0; i < PAGE_SIZE / 128; i++) {
		unsigned char signature = 0;
		for (j = 0; j < 32; j++) {
			signature = (signature + *p++) & 0xff;
		}
		printk("%02x", signature);
	}
	printk("\n");
}

void print_page_signature_pid(pid_t pid, unsigned char *addr)
{
	printk("  [%d] ", pid);
	print_page_signature(addr);
}

static DEFINE_SPINLOCK(__print_lock);
static char *__print_buffer = NULL;

void print_page_owner(unsigned long addr, unsigned long *owners, pid_t pid)
{
	if (unlikely(!__print_buffer)) {
		__print_buffer = kmalloc(PAGE_SIZE, GFP_KERNEL);
	}
	spin_lock(&__print_lock);
	bitmap_print_to_pagebuf(
			true, __print_buffer, owners, MAX_POPCORN_NODES);
	printk("  [%d] %lx %s", pid, addr, __print_buffer);
	spin_unlock(&__print_lock);
}

#include <linux/fs.h>

static DEFINE_SPINLOCK(__file_path_lock);
static char *__file_path_buffer = NULL;

int get_file_path(struct file *file, char *sz, size_t size)
{
	char *ppath;
	int retval = 0;

	if (!file) {
		BUG_ON(size < 1);
		sz[0] = '\0';
		return -EINVAL;
	}

	if (unlikely(!__file_path_buffer)) {
		__file_path_buffer = kmalloc(PAGE_SIZE, GFP_KERNEL);
	}

	spin_lock(&__file_path_lock);
	ppath = file_path(file, __file_path_buffer, PAGE_SIZE);
	if (IS_ERR(ppath)) {
		retval = -ESRCH;
		goto out_unlock;
	}

	strncpy(sz, ppath, size);

out_unlock:
	spin_unlock(&__file_path_lock);
	return 0;
}


static const char *__comm_to_trace[] = {
};

#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/ptrace.h>

void trace_task_status(void)
{
	int i;
	for (i = 0; i < ARRAY_SIZE(__comm_to_trace); i++) {
		const char *comm = __comm_to_trace[i];
		if (memcmp(current->comm, comm, strlen(comm)) == 0) {
			printk("@@[%d] %s %lx\n", current->pid,
					current->comm, instruction_pointer(current_pt_regs()));
			break;
		}
	}
}
