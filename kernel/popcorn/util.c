#include <linux/mm.h>
#include <linux/slab.h>

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

#include "types.h"
#include <linux/fdtable.h>
#include <popcorn/debug.h>

int clone_fdtable(fd_t *fds, struct files_struct *files)
{
	struct fdtable *fdtab;
	struct file *filep;
	struct path file_path;
	int i = 0;
	char buf[128];
	char *cwd;
	unsigned int fd_cnt = 0;

	spin_lock(&files->file_lock);

	fdtab = files_fdtable(files);
	BUG_ON(fdtab == NULL);

	// # of fd table entries
	fd_cnt = fdtab->max_fds;
	PSPRINTK("fdtable max_fds: %d, fds size total: %ld. fds size: %ld\n",
		fd_cnt, sizeof(*fds)*fd_cnt, sizeof(*fds));

	// zero out the fds
	memset(fds, 0, sizeof(*fds)*64);
#if 1
	while (i < 64) {
		filep = fdtab->fd[i];
		//fds[i].idx = -1;
		//memset(&fds[i], 0, sizeof(*fds));
		if (filep) {
			file_path = filep->f_path;
			cwd = d_path(&file_path, buf, 128);	// convert a dentry into an ASCII path name
			fds[i].idx = i;
			memcpy(fds[i].file_path, cwd, strlen(cwd));
			PSPRINTK("File fd: %d, path: %s, path len: %ld\n",
				i, cwd, strlen(cwd));
		}
		i++;
	}
#endif
	spin_unlock(&files->file_lock);

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
