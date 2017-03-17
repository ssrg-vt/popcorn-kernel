/**
 * @file remote_cpu_info.c
 *
 * Popcorn Linux remote cpuinfo implementation
 * This work is a rework of Akshay Giridhar and Sharath Kumar Bhat's
 * implementation to provide the proc/cpuinfo for remote cores.
 *
 * @author Jingoo Han, SSRG Virginia Tech 2017
 */

#include <linux/printk.h>
#include <linux/slab.h>
#include <linux/seq_file.h>
#include <linux/sched.h>
#include <linux/wait.h>

#include <popcorn/bundle.h>
#include <popcorn/cpuinfo.h>
#include <popcorn/pcn_kmsg.h>

#define REMOTE_CPUINFO_VERBOSE 1
#if REMOTE_CPUINFO_VERBOSE
#define CPUPRINTK(...) printk(__VA_ARGS__)
#else
#define CPUPRINTK(...)
#endif

static DECLARE_WAIT_QUEUE_HEAD(wq_cpu);
static int wait_cpu_list = -1;

struct remote_cpu_info_message {
	struct pcn_kmsg_hdr header;
	struct _remote_cpu_info_data cpu_info_data;
	int nid;
} __packed __aligned(64);

static struct _remote_cpu_info_data *saved_cpu_info[MAX_POPCORN_NODES];

int send_remote_cpu_info_request(unsigned int nid)
{
	struct remote_cpu_info_message *request;
	int ret = 0;

	CPUPRINTK("%s: Entered, nid: %d\n", __func__, nid);

	wait_cpu_list = -1;

	request = kzalloc(sizeof(*request), GFP_KERNEL);

	/* 1. Construct request data to send it into remote node */

	/* 1-1. Fill the header information */
	request->header.type = PCN_KMSG_TYPE_REMOTE_PROC_CPUINFO_REQUEST;
	request->header.prio = PCN_KMSG_PRIO_NORMAL;
	request->nid = my_nid;

	/* 1-2. Fill the machine-dependent CPU infomation */
	ret = fill_cpu_info(&request->cpu_info_data);
	if (ret < 0) {
		CPUPRINTK("%s: failed to fill cpu info\n", __func__);
		goto out;
	}

	/* 1-3. Send request into remote node */
	ret = pcn_kmsg_send_long(nid, request, sizeof(*request));
	if (ret < 0) {
		CPUPRINTK("%s: failed to send request message\n", __func__);
		goto out;
	}

	/* 2. Request message should wait until response message is done. */
	wait_event_interruptible(wq_cpu, wait_cpu_list != -1);
	wait_cpu_list = -1;

	CPUPRINTK("%s: done\n", __func__);
out:
	kfree(request);
	return 0;
}

unsigned int get_number_cpus_from_remote_node(unsigned int nid)
{
	unsigned int num_cpus = 0;

	switch (saved_cpu_info[nid]->arch_type) {
	case arch_x86:
		num_cpus = saved_cpu_info[nid]->arch.x86.num_cpus;
		break;
	case arch_arm:
		/* TODO */
		break;
	default:
		CPUPRINTK("%s: Unknown CPU\n", __func__);
		num_cpus = 0;
		break;
	}

	return num_cpus;
}

static int handle_remote_cpu_info_request(struct pcn_kmsg_message *inc_msg)
{
	struct remote_cpu_info_message *request;
	struct remote_cpu_info_message *response;
	int ret;

	CPUPRINTK("%s: Entered\n", __func__);

	request = (struct remote_cpu_info_message *)inc_msg;
	if (request == NULL) {
		CPUPRINTK("%s: NULL pointer\n", __func__);
		return -EINVAL;
	}

	response = kzalloc(sizeof(*response), GFP_KERNEL);

	/* 1. Save remote cpu info from remote node */
	memcpy(saved_cpu_info[request->nid],
	       &request->cpu_info_data, sizeof(request->cpu_info_data));

	/* 2. Construct response data to send it into remote node */

	/* 2-1. Fill the header information */
	response->header.type = PCN_KMSG_TYPE_REMOTE_PROC_CPUINFO_RESPONSE;
	response->header.prio = PCN_KMSG_PRIO_NORMAL;
	response->nid = my_nid;

	/* 2-2. Fill the machine-dependent CPU infomation */
	ret = fill_cpu_info(&response->cpu_info_data);
	if (ret < 0) {
		CPUPRINTK("%s: failed to fill cpu info\n", __func__);
		goto out;
	}

	/* 2-3. Send response into remote node */
	ret = pcn_kmsg_send_long(request->nid, response, sizeof(*response));
	if (ret < 0) {
		CPUPRINTK("%s: failed to send response message\n", __func__);
		goto out;
	}

	/* 3. Remove request message received from remote node */
	pcn_kmsg_free_msg(request);

	CPUPRINTK("%s: done\n", __func__);
out:
	kfree(response);
	return 0;
}

static int handle_remote_cpu_info_response(struct pcn_kmsg_message *inc_msg)
{
	struct remote_cpu_info_message *response;

	CPUPRINTK("%s: Entered\n", __func__);

	wait_cpu_list = 1;

	response = (struct remote_cpu_info_message *)inc_msg;
	if (response == NULL) {
		CPUPRINTK("%s: NULL pointer\n", __func__);
		return -EINVAL;
	}

	/* 1. Save remote cpu info from remote node */
	memcpy(saved_cpu_info[response->nid],
	       &response->cpu_info_data, sizeof(response->cpu_info_data));

	/* 2. Wake up request message waiting */
	wake_up_interruptible(&wq_cpu);

	/* 3. Remove response message received from remote node */
	pcn_kmsg_free_msg(response);

	CPUPRINTK("%s: done\n", __func__);
	return 0;
}

int remote_cpu_info_init(void)
{
	int i = 0;

	/* Allocate the buffer for saving remote CPU info */
	for (i = 0; i < MAX_POPCORN_NODES; i++)
		saved_cpu_info[i] = kzalloc(sizeof(struct _remote_cpu_info_data),
					    GFP_KERNEL);

	/* Register callbacks for both request and response */
	pcn_kmsg_register_callback(PCN_KMSG_TYPE_REMOTE_PROC_CPUINFO_REQUEST,
				   handle_remote_cpu_info_request);
	pcn_kmsg_register_callback(PCN_KMSG_TYPE_REMOTE_PROC_CPUINFO_RESPONSE,
				   handle_remote_cpu_info_response);

	CPUPRINTK("%s: done\n", __func__);
	return 0;
}

static void print_x86_cpuinfo(struct seq_file *m,
		       struct _remote_cpu_info_data *data,
		       int count)
{
	seq_printf(m, "processor\t: %u\n", data->arch.x86.cpu[count]._processor);
	seq_printf(m, "vendor_id\t: %s\n", data->arch.x86.cpu[count]._vendor_id);
	seq_printf(m, "cpu_family\t: %d\n", data->arch.x86.cpu[count]._cpu_family);
	seq_printf(m, "model\t\t: %u\n", data->arch.x86.cpu[count]._model);
	seq_printf(m, "model name\t: %s\n", data->arch.x86.cpu[count]._model_name);

	if (data->arch.x86.cpu[count]._stepping != -1)
		seq_printf(m, "stepping\t: %d\n", data->arch.x86.cpu[count]._stepping);
	else
		seq_puts(m, "stepping\t: unknown\n");

	seq_printf(m, "microcode\t: 0x%lx\n", data->arch.x86.cpu[count]._microcode);
	seq_printf(m, "cpu MHz\t\t: %u\n", data->arch.x86.cpu[count]._cpu_freq);
	seq_printf(m, "cache size\t: %d kB\n", data->arch.x86.cpu[count]._cache_size);
	seq_puts(m, "flags\t\t:");
	seq_printf(m, " %s", data->arch.x86.cpu[count]._flags);
	seq_printf(m, "\nbogomips\t: %lu\n", data->arch.x86.cpu[count]._nbogomips);
	seq_printf(m, "TLB size\t: %d 4K pages\n", data->arch.x86.cpu[count]._TLB_size);
	seq_printf(m, "clflush size\t: %u\n", data->arch.x86.cpu[count]._clflush_size);
	seq_printf(m, "cache_alignment\t: %d\n", data->arch.x86.cpu[count]._cache_alignment);
	seq_printf(m, "address sizes\t: %u bits physical, %u bits virtual\n",
		   data->arch.x86.cpu[count]._bits_physical,
		   data->arch.x86.cpu[count]._bits_virtual);
}

static void print_unknown_cpuinfo(struct seq_file *m)
{
	seq_puts(m, "processor\t: Unknown\n");
	seq_puts(m, "vendor_id\t: Unknown\n");
	seq_puts(m, "cpu_family\t: Unknown\n");
	seq_puts(m, "model\t\t: Unknown\n");
	seq_puts(m, "model name\t: Unknown\n");
}

int remote_proc_cpu_info(struct seq_file *m, unsigned int nid, unsigned int vpos)
{
	seq_puts(m, "****    Remote CPU    ****\n");

	switch (saved_cpu_info[nid]->arch_type) {
	case arch_x86:
		print_x86_cpuinfo(m, saved_cpu_info[nid], vpos);
		break;
	case arch_arm:
		/* TODO */
		break;
	default:
		print_unknown_cpuinfo(m);
		break;
	}

	seq_puts(m, "\n");
	return 0;
}
