/**
 * @file sched_server.c
 *
 * Popcorn Linux scheduler server implementation
 * This server provides the functionalities to communicate current load, power
 * consumption and other parameters of interest of the scheduler to all the
 * kernels. Moreover the server communicates to user space the decision to
 * migrate (see mtrig). Note that this is also implemented in fs/proc/task_mmu.c
 * if the decision is per process. All these functions are now accessible via
 * the /proc interface.
 * This is specifically for ARM/x86, actually X-Gene 1 and Intel (any Intel
 * processor with RAPL readings).
 *
 * @author Vincent Legout, Antonio Barbalace, SSRG Virginia Tech 2016
 */

#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/slab.h>
#include <linux/proc_fs.h>
#include <asm/uaccess.h>
 
#include <linux/cpu_namespace.h>

#include <linux/pcn_kmsg.h>
#include <linux/process_server.h>
#include <popcorn/process_server.h>

#include "internal.h"

///////////////////////////////////////////////////////////////////////////////
// Vincent's scheduling infrasrtucture based on Antonio's power/pmu readings
///////////////////////////////////////////////////////////////////////////////
#define POPCORN_POWER_N_VALUES 10
int *popcorn_power_x86_1;
int *popcorn_power_x86_2;
int *popcorn_power_arm_1;
int *popcorn_power_arm_2;
int *popcorn_power_arm_3;
EXPORT_SYMBOL_GPL(popcorn_power_x86_1);
EXPORT_SYMBOL_GPL(popcorn_power_x86_2);
EXPORT_SYMBOL_GPL(popcorn_power_arm_1);
EXPORT_SYMBOL_GPL(popcorn_power_arm_2);
EXPORT_SYMBOL_GPL(popcorn_power_arm_3);

struct popcorn_sched {
	int **power_arm;
	int **power_x86;

	struct task_struct **tasks;
};

//struct popcorn_sched pop_sched;
//EXPORT_SYMBOL_GPL(pop_sched);

///////////////////////////////////////////////////////////////////////////////
// scheduling stuff
///////////////////////////////////////////////////////////////////////////////
static int popcorn_sched_sync(void *_param)
{
	int cpu;
	sched_periodic_req req;

#if defined(CONFIG_ARM64)
	cpu = 1;
#else
	cpu = 0;
#endif

	while (1) {
		usleep_range(200000, 250000); // total time on ARM is currently around 100ms (not busy waiting)

		req.header.type = PCN_KMSG_TYPE_SCHED_PERIODIC;
		req.header.prio = PCN_KMSG_PRIO_NORMAL;

#if defined(CONFIG_ARM64)
		req.power_1 = popcorn_power_arm_1[POPCORN_POWER_N_VALUES - 1];
		req.power_2 = popcorn_power_arm_2[POPCORN_POWER_N_VALUES - 1];
		req.power_3 = popcorn_power_arm_3[POPCORN_POWER_N_VALUES - 1];
#else
		req.power_1 = popcorn_power_x86_1[POPCORN_POWER_N_VALUES - 1];
		req.power_2 = popcorn_power_x86_2[POPCORN_POWER_N_VALUES - 1];
		req.power_3 = 0;
#endif

		pcn_kmsg_send_long(cpu, (struct pcn_kmsg_long_message*) &req,
				sizeof(sched_periodic_req) - sizeof(struct pcn_kmsg_hdr));
	}
	return 0;
}

static int handle_sched_periodic(struct pcn_kmsg_message *inc_msg)
{
	sched_periodic_req *req = (sched_periodic_req *)inc_msg;

#if defined(CONFIG_ARM64)
	popcorn_power_x86_1[POPCORN_POWER_N_VALUES - 1] = req->power_1;
	popcorn_power_x86_2[POPCORN_POWER_N_VALUES - 1] = req->power_2;
	//        popcorn_power_x86_3[POPCORN_POWER_N_VALUES - 1] = req->power_3;
#else
	popcorn_power_arm_1[POPCORN_POWER_N_VALUES - 1] = req->power_1;
	popcorn_power_arm_2[POPCORN_POWER_N_VALUES - 1] = req->power_2;
	popcorn_power_arm_3[POPCORN_POWER_N_VALUES - 1] = req->power_3;
#endif
	/*printk("power: %d %d %d\n", req->power_1, req->power_2, req->power_3);*/

	pcn_kmsg_free_msg(inc_msg);
	return 0;
}


static ssize_t power_read(struct file *file, char __user *buf, size_t count, loff_t *ppos)
{
	int ret, len = 0;
	char buffer[256] = {0};
	if (*ppos > 0)
		return 0; //EOF

	len += snprintf(buffer, sizeof(buffer) - 1,
			"ARM\t%d\t%d\t%d\n",
			popcorn_power_arm_1[POPCORN_POWER_N_VALUES - 1],
			popcorn_power_arm_2[POPCORN_POWER_N_VALUES - 1],
			popcorn_power_arm_3[POPCORN_POWER_N_VALUES - 1]);
	len += snprintf((buffer + len), sizeof(buffer) - len - 1,
			"x86\t%d\t%d\n",
			popcorn_power_x86_1[POPCORN_POWER_N_VALUES - 1],
			popcorn_power_x86_2[POPCORN_POWER_N_VALUES - 1]);

	if (count < len)
		len = count;
	ret = copy_to_user(buf, buffer, len);

	*ppos += len;
	return len;
}

static const struct file_operations power_fops = {
	.owner = THIS_MODULE,
	.read = power_read,
};


///////////////////////////////////////////////////////////////////////////////
// Global VDSO Support (to be removed)
///////////////////////////////////////////////////////////////////////////////
long tell_migration = 0;
static ssize_t mtrig_write (struct file *file, const char __user *buf, size_t count, loff_t *ppos)
{
	int ret, len;
	char buffer[8];
	memset(buffer, 0, sizeof(buffer));
	len = count > sizeof(buffer) ? sizeof(buffer) : count;

	ret = copy_from_user(buffer, buf, len);
	tell_migration = simple_strtol(buffer, NULL, 0);

	suggest_migration((int)tell_migration);
	//printk("%s: suggest_migration %d (%d,%d,%d)\n", __func__, (int)tell_migration, ret, (int) count, len);

	return len;
}

static ssize_t mtrig_read(struct file *file, char __user *buf, size_t count, loff_t *ppos)
{
	int ret, len;
	char buffer[8];
	memset(buffer, 0, sizeof(buffer));
	if (*ppos > 0)
		return 0; //EOF

	len = snprintf(buffer, sizeof(buffer), "%d", (int)tell_migration);
	if (count < len)
		len = count;
	ret = copy_to_user(buf, buffer, len);
	//printk("%s: tell_migration %d (%d,%d,%d,%ld)\n", __func__, (int)tell_migration, ret, len, count, (long)*ppos);

	*ppos += len;
	return len;
}

static const struct file_operations mtrig_fops = {
	.owner = THIS_MODULE,
	.read = mtrig_read,
	.write = mtrig_write,
};


///////////////////////////////////////////////////////////////////////////////
// List of Popcorn processes
///////////////////////////////////////////////////////////////////////////////

// CPU load per thread
static void popcorn_ps_load(struct task_struct * t, unsigned int *puload, unsigned int *psload)
{
	unsigned long delta, now;
	unsigned long utime = cputime_to_jiffies(t->utime);
	unsigned long stime = cputime_to_jiffies(t->stime);
	unsigned int uload, sload;
	delta = now = get_jiffies_64();

	if (!t->llasttimestamp)
		delta -= nsecs_to_jiffies(t->real_start_time);
	else
		delta -= t->llasttimestamp;

	if (delta == 0) { // TODO fix the following
		uload = 100;
		sload = 100;
	}
	else {
		uload = ((utime - t->lutime) * 100) / delta;
		sload = ((stime - t->lstime) * 100) / delta;
	}

	t->llasttimestamp = now;
	t->lutime = utime;
	t->lstime = stime;

	if (puload)
		*puload = uload;
	if (psload)
		*psload = sload;

	return;
}


#define PROC_BUFFER_PS 8192
static ssize_t popcorn_ps_read(struct file *file, char __user *buf, size_t count, loff_t *ppos)
{
	int ret, len = 0, written = 0, i;
	char * buffer;
	memory_t ** lista;

	lista = (memory_t **)kmalloc(sizeof(memory_t*) * 1024, GFP_KERNEL);
	if (!lista)
		return 0; // error
	memset(lista, 0, (sizeof(memory_t*) * 1024));
	ret = dump_memory_entries(lista, 1024, &written);
	//if (!ret)
	//printk("%s: WARN: there are more memory_t entries than %d\n", __func__, written);

	buffer = kmalloc(PROC_BUFFER_PS, GFP_KERNEL);
	if (!buffer)
		return 0; // error
	memset(buffer, 0, PROC_BUFFER_PS);

	if (*ppos > 0)
		return 0; //EOF

	for (i = 0; i < written; i++) {
		struct task_struct * t;
		struct task_struct * ppp = lista[i]->main;

		len += snprintf((buffer +len), PROC_BUFFER_PS - len,
				"%s %d:%d:%d:%lu", ppp->comm,
				ppp->tgroup_home_cpu, ppp->tgroup_home_id, ppp->tgroup_distributed,
				ppp->mm->total_vm); // this is in number of pages

		t = ppp;
		do {
			// here I want to list only user/kernel threads
			if (t->main) {
				// this is the main thread (kernel space only) nothing to do
			} else {
				if (t->executing_for_remote == 0 && t->distributed_exit== EXIT_NOT_ACTIVE) {
					// this is the nothing to fo
				} else {
					// TODO print only the one that are currently running (not migrated!)

					// CPU load per thread
					unsigned int uload, sload;
					popcorn_ps_load(t, &uload, &sload);

					len += snprintf((buffer +len), PROC_BUFFER_PS -len,
							" %d:%d:%d:%d:%d %d:%d;",
							(int)t->pid, t->represents_remote, t->executing_for_remote, t->main, t->distributed_exit,
							uload, sload); //these are in percentage
				}
			}
		} while_each_thread(ppp, t);

		len += snprintf((buffer +len), PROC_BUFFER_PS - len, "\n");
	}
	if (written == 0)
		len += snprintf(buffer, PROC_BUFFER_PS, "none\n");

	if (count < len)
		len = count;
	ret = copy_to_user(buf, buffer, len);

	*ppos += len;
	return len;
}

static const struct file_operations popcorn_ps_fops = {
	.owner = THIS_MODULE,
	.read = popcorn_ps_read,
};

static ssize_t popcorn_ps_read1 (struct file *file, char __user *buf, size_t count, loff_t *ppos)
{
	int ret, len = 0;
	char * buffer;
	struct task_struct * ppp;

	buffer = kmalloc(PROC_BUFFER_PS, GFP_KERNEL);
	if (!buffer)
		return 0; // error
	memset(buffer, 0, PROC_BUFFER_PS);

	if (*ppos > 0)
		return 0; //EOF

	if (popcorn_ns == 0) {
		printk("%s: popcorn_ns is ZERO\n", __func__);
		return 0;
	}

	for_each_process(ppp) {
		/* NOTEs
		 * All process in the popcorn namespace can migrate, however it doesn't have sense to migrate
		 * init (in fact we should check that any request of migrating init will return error)
		 * and it doesn't make much sense to migrate a shell (for other reasons tho).
		 */

		if ( ppp && (ppp->nsproxy) && (ppp->nsproxy->cpu_ns == popcorn_ns) ) {
			struct task_struct * t;

			len += snprintf((buffer +len), PROC_BUFFER_PS - len,
					"%s %d:%d:%d:%lu", ppp->comm,
					ppp->tgroup_home_cpu, ppp->tgroup_home_id, ppp->tgroup_distributed,
					ppp->mm ? ppp->mm->total_vm : -1); // this is in number of pages

			/* NOTEs
			 * A Popcorn process is a mix of different threads and Popcorn uses different tricks
			 * to speed up migrations, one of those is setting up a pool of threads (Marina's pull)
			 * and another is to use a kernel thread called main_for_distributed_kernel_thread. The threads
			 * in the pool of threads are sleeping in a function sleep_shadow, thus called shadow threads.
			 */
			t = ppp;
			do {
				// here I want to list only user/kernel threads
				if (t->main) {
					// this is the main thread (kernel space only) nothing to do
				}
				else {
					if (t->executing_for_remote == 0 && t->distributed_exit== EXIT_NOT_ACTIVE) {
						// this is the nothing to fo
					}
					else {
						// TODO print only the one that are currently running (not migrated!)
						// CPU load per thread
						unsigned int uload, sload;
						popcorn_ps_load(t, &uload, &sload);

						len += snprintf((buffer +len), PROC_BUFFER_PS -len,
								" %d:%d:%d:%d:%d %d:%d;",
								(int)t->pid, t->represents_remote, t->executing_for_remote, t->main, t->distributed_exit,
								uload, sload); //these are in percentage
					}
				}
			} while_each_thread(ppp, t);

			len += snprintf((buffer +len), PROC_BUFFER_PS - len, "\n");
		}
	}

	if (count < len)
		len = count;
	ret = copy_to_user(buf, buffer, len);

	*ppos += len;
	return len;
}

static const struct file_operations popcorn_ps_fops1 = {
	.owner = THIS_MODULE,
	.read = popcorn_ps_read1,
};


int __init sched_server_init(void)
{
	struct task_struct *kt_sched;
	struct proc_dir_entry *res;
	int i;
	int ret;
	if ((ret = pcn_kmsg_register_callback(PCN_KMSG_TYPE_SCHED_PERIODIC, handle_sched_periodic)))
		return ret;

	popcorn_power_x86_1 = (int *) kmalloc(POPCORN_POWER_N_VALUES * sizeof(int), GFP_ATOMIC);
	popcorn_power_x86_2 = (int *) kmalloc(POPCORN_POWER_N_VALUES * sizeof(int), GFP_ATOMIC);
	popcorn_power_arm_1 = (int *) kmalloc(POPCORN_POWER_N_VALUES * sizeof(int), GFP_ATOMIC);
	popcorn_power_arm_2 = (int *) kmalloc(POPCORN_POWER_N_VALUES * sizeof(int), GFP_ATOMIC);
	popcorn_power_arm_3 = (int *) kmalloc(POPCORN_POWER_N_VALUES * sizeof(int), GFP_ATOMIC);
	if (!popcorn_power_x86_1 || !popcorn_power_x86_1 ||
			!popcorn_power_arm_1 || !popcorn_power_arm_2 || !popcorn_power_arm_3)
		return -ENOMEM;

	for (i = 0; i < POPCORN_POWER_N_VALUES; i++) {
		popcorn_power_x86_1[i] = 0;
		popcorn_power_x86_2[i] = 0;
		popcorn_power_arm_1[i] = 0;
		popcorn_power_arm_2[i] = 0;
		popcorn_power_arm_3[i] = 0;
	}

	kt_sched = kthread_run(popcorn_sched_sync, NULL, "popcorn_sched_sync");
	if (IS_ERR(kt_sched)) {
		printk(KERN_ERR"%s: Cannot create popcorn_sched_sync thread\n", __func__);
		return (int)PTR_ERR(kt_sched);
	}

	res = proc_create("power", S_IRUGO, NULL, &power_fops);
	if (!res)
		printk("%s: WARNING: failed to create proc entry for power\n", __func__);

	res = proc_create("mtrig", S_IRUGO, NULL, &mtrig_fops);
	if (!res)
		printk("%s: WARNING: failed to create proc entry for triggering migrations\n", __func__);

	res = proc_create("popcorn_ps", S_IRUGO, NULL, &popcorn_ps_fops);
	if (!res)
		printk("%s: WARNING: failed to create proc entry for Popcorn process list\n", __func__);

	res = proc_create("popcorn_ps1", S_IRUGO, NULL, &popcorn_ps_fops1);
	if (!res)
		printk("%s: WARNING: failed to create proc entry for Popcorn process list 1\n", __func__);

	printk(KERN_INFO"%s: done\n", __func__);
	return 0;
}
