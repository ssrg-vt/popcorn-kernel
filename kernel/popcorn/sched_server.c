/**
 * @file sched_server.c
 *
 * Popcorn Linux scheduler server implementation
 * This server provides the functionalities to communicate current load, power
 * consumption and other parameters of interest of the scheduler to all the
 * kernels. Note that this is also implemented in fs/proc/task_mmu.c
 * if the decision is per process. All these functions are now accessible via
 * the /proc interface.
 * This is specifically for ARM/x86, actually X-Gene 1 and Intel (any Intel
 * processor with RAPL readings).
 *
 * @author Vincent Legout, Antonio Barbalace, SSRG Virginia Tech 2016
 * @author Sang-Hoon Kim, SSRG Virginia Tech 2017
 */

#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/slab.h>
#include <linux/proc_fs.h>
#include <asm/uaccess.h>

#include <linux/cpu_namespace.h>

#include <popcorn/pcn_kmsg.h>
#include <popcorn/process_server.h>

#include "types.h"

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


///////////////////////////////////////////////////////////////////////////////
// scheduling stuff
///////////////////////////////////////////////////////////////////////////////

static int popcorn_sched_sync(void *_param)
{
	sched_periodic_req req;

	while (!kthread_should_stop()) {
		// total time on ARM is currently around 100ms (not busy waiting)
		usleep_range(200000, 250000);

		req.header.type = PCN_KMSG_TYPE_SCHED_PERIODIC;
		req.header.prio = PCN_KMSG_PRIO_NORMAL;

#ifdef CONFIG_POWER_SENSOR_ARM
		req.power_1 = popcorn_power_arm_1[POPCORN_POWER_N_VALUES - 1];
		req.power_2 = popcorn_power_arm_2[POPCORN_POWER_N_VALUES - 1];
		req.power_3 = popcorn_power_arm_3[POPCORN_POWER_N_VALUES - 1];
#endif
#ifdef CONFIG_POWER_SENSOR_X86
		req.power_1 = popcorn_power_x86_1[POPCORN_POWER_N_VALUES - 1];
		req.power_2 = popcorn_power_x86_2[POPCORN_POWER_N_VALUES - 1];
		req.power_3 = 0;
#endif

#if 0 // beowulf
		pcn_kmsg_send_long(other, &req, sizeof(req));
#endif
	}

	return 0;
}

static int handle_sched_periodic(struct pcn_kmsg_message *inc_msg)
{
#if defined(CONFIG_POWER_SENSOR_X86) || defined(CONFIG_POWER_SENSOR_ARM)
	sched_periodic_req *req = (sched_periodic_req *)inc_msg;

	// printk("Power: %d %d %d\n", req->power_1, req->power_2, req->power_3);

#ifdef CONFIG_POWER_SENSOR_ARM
	popcorn_power_x86_1[POPCORN_POWER_N_VALUES - 1] = req->power_1;
	popcorn_power_x86_2[POPCORN_POWER_N_VALUES - 1] = req->power_2;
//	popcorn_power_x86_3[POPCORN_POWER_N_VALUES - 1] = req->power_3;
#endif
#ifdef CONFIG_POWER_SENSOR_X86
	popcorn_power_arm_1[POPCORN_POWER_N_VALUES - 1] = req->power_1;
	popcorn_power_arm_2[POPCORN_POWER_N_VALUES - 1] = req->power_2;
	popcorn_power_arm_3[POPCORN_POWER_N_VALUES - 1] = req->power_3;
#endif
#endif

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
// List of Popcorn processes
///////////////////////////////////////////////////////////////////////////////

// CPU load per thread
static void popcorn_ps_load(struct task_struct *t, unsigned int *puload, unsigned int *psload)
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
	int ret, len = 0;
	char * buffer;
	struct task_struct *p;

	buffer = kzalloc(PROC_BUFFER_PS, GFP_KERNEL);
	if (!buffer)
		return 0; // error

	if (*ppos > 0)
		return 0; //EOF

	for_each_process(p) {
		if (process_is_distributed(p)) {
			struct task_struct *t;

			if (p->is_vma_worker && !p->at_remote) continue;

			len += snprintf((buffer + len), PROC_BUFFER_PS - len,
					"%c: %16s %5d             %lu\n",
					p->at_remote ? 'R' : 'L',
					p->comm,
					p->pid,
					p->mm->total_vm); // in # of pages

			for_each_thread(p, t) {
				unsigned int uload, sload;

				if (p->at_remote && t->is_vma_worker) continue;

				// CPU load per thread
				popcorn_ps_load(t, &uload, &sload);

				len += snprintf((buffer +len), PROC_BUFFER_PS - len,
						"                    %5d %5d %5d %d %d\n",
						t->pid, t->origin_nid, t->origin_pid,
						uload, sload); // in %
			}
		}
	}

	if (count < len)
		len = count;
	ret = copy_to_user(buf, buffer, len);
	*ppos += len;

	kfree(buffer);
	return len;
}

static const struct file_operations popcorn_ps_fops = {
	.owner = THIS_MODULE,
	.read = popcorn_ps_read,
};


int __init sched_server_init(void)
{
	struct task_struct *kt_sched;
	struct proc_dir_entry *res;
	int i;

	pcn_kmsg_register_callback(PCN_KMSG_TYPE_SCHED_PERIODIC, handle_sched_periodic);

	popcorn_power_arm_1 = kmalloc(POPCORN_POWER_N_VALUES * sizeof(int), GFP_KERNEL);
	popcorn_power_arm_2 = kmalloc(POPCORN_POWER_N_VALUES * sizeof(int), GFP_KERNEL);
	popcorn_power_arm_3 = kmalloc(POPCORN_POWER_N_VALUES * sizeof(int), GFP_KERNEL);
	popcorn_power_x86_1 = kmalloc(POPCORN_POWER_N_VALUES * sizeof(int), GFP_KERNEL);
	popcorn_power_x86_2 = kmalloc(POPCORN_POWER_N_VALUES * sizeof(int), GFP_KERNEL);

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
		printk("Failed to create proc entry for power monitoring\n");

	res = proc_create("popcorn_ps", S_IRUGO, NULL, &popcorn_ps_fops);
	if (!res)
		printk("Failed to create proc entry for process list\n");

	printk(KERN_INFO"%s: done\n", __func__);
	return 0;
}
