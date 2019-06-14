// SPDX-License-Identifier: GPL-2.0
#include <linux/smp.h>
#include <linux/timex.h>
#include <linux/string.h>
#include <linux/seq_file.h>
#include <linux/cpufreq.h>
#include <popcorn/bundle.h>

#include "cpu.h"

extern void send_remote_cpu_info_request(unsigned int nid);
extern unsigned int get_number_cpus_from_remote_node(unsigned int nid);
extern int remote_proc_cpu_info(struct seq_file *m, unsigned int nid,
				unsigned int vpos);

static struct cpu_global_info {
	unsigned int remote;
	struct cpuinfo_x86 *c;
	unsigned int vpos;
	unsigned int nid;
} cpu_global_info;

static struct cpuinfo_x86 c;

/*
 * num_cpus: # of cores of each nodes
 * num_total_cpus: # of total cpus of all connected nodes
 */
static unsigned int num_cpus[MAX_POPCORN_NODES];
static unsigned int num_total_cpus;

/*
 *	Get CPU information for use by the procfs.
 */
static void show_cpuinfo_core(struct seq_file *m, struct cpuinfo_x86 *c,
			      unsigned int cpu)
{
#ifdef CONFIG_SMP
	seq_printf(m, "physical id\t: %d\n", c->phys_proc_id);
	seq_printf(m, "siblings\t: %d\n",
		   cpumask_weight(topology_core_cpumask(cpu)));
	seq_printf(m, "core id\t\t: %d\n", c->cpu_core_id);
	seq_printf(m, "cpu cores\t: %d\n", c->booted_cores);
	seq_printf(m, "apicid\t\t: %d\n", c->apicid);
	seq_printf(m, "initial apicid\t: %d\n", c->initial_apicid);
#endif
}

#ifdef CONFIG_X86_32
static void show_cpuinfo_misc(struct seq_file *m, struct cpuinfo_x86 *c)
{
	seq_printf(m,
		   "fdiv_bug\t: %s\n"
		   "f00f_bug\t: %s\n"
		   "coma_bug\t: %s\n"
		   "fpu\t\t: %s\n"
		   "fpu_exception\t: %s\n"
		   "cpuid level\t: %d\n"
		   "wp\t\t: yes\n",
		   boot_cpu_has_bug(X86_BUG_FDIV) ? "yes" : "no",
		   boot_cpu_has_bug(X86_BUG_F00F) ? "yes" : "no",
		   boot_cpu_has_bug(X86_BUG_COMA) ? "yes" : "no",
		   boot_cpu_has(X86_FEATURE_FPU) ? "yes" : "no",
		   boot_cpu_has(X86_FEATURE_FPU) ? "yes" : "no",
		   c->cpuid_level);
}
#else
static void show_cpuinfo_misc(struct seq_file *m, struct cpuinfo_x86 *c)
{
	seq_printf(m,
		   "fpu\t\t: yes\n"
		   "fpu_exception\t: yes\n"
		   "cpuid level\t: %d\n"
		   "wp\t\t: yes\n",
		   c->cpuid_level);
}
#endif

static int show_cpuinfo(struct seq_file *m, void *v)
{
	struct cpuinfo_x86 *c = v;
	unsigned int cpu;
	int i;

	cpu = c->cpu_index;
	seq_printf(m, "processor\t: %u\n"
		   "vendor_id\t: %s\n"
		   "cpu family\t: %d\n"
		   "model\t\t: %u\n"
		   "model name\t: %s\n",
		   cpu,
		   c->x86_vendor_id[0] ? c->x86_vendor_id : "unknown",
		   c->x86,
		   c->x86_model,
		   c->x86_model_id[0] ? c->x86_model_id : "unknown");

	if (c->x86_stepping || c->cpuid_level >= 0)
		seq_printf(m, "stepping\t: %d\n", c->x86_stepping);
	else
		seq_puts(m, "stepping\t: unknown\n");
	if (c->microcode)
		seq_printf(m, "microcode\t: 0x%x\n", c->microcode);

	if (cpu_has(c, X86_FEATURE_TSC)) {
		unsigned int freq = aperfmperf_get_khz(cpu);

		if (!freq)
			freq = cpufreq_quick_get(cpu);
		if (!freq)
			freq = cpu_khz;
		seq_printf(m, "cpu MHz\t\t: %u.%03u\n",
			   freq / 1000, (freq % 1000));
	}

	/* Cache size */
	if (c->x86_cache_size)
		seq_printf(m, "cache size\t: %u KB\n", c->x86_cache_size);

	show_cpuinfo_core(m, c, cpu);
	show_cpuinfo_misc(m, c);

	seq_puts(m, "flags\t\t:");
	for (i = 0; i < 32*NCAPINTS; i++)
		if (cpu_has(c, i) && x86_cap_flags[i] != NULL)
			seq_printf(m, " %s", x86_cap_flags[i]);

	seq_puts(m, "\nbugs\t\t:");
	for (i = 0; i < 32*NBUGINTS; i++) {
		unsigned int bug_bit = 32*NCAPINTS + i;

		if (cpu_has_bug(c, bug_bit) && x86_bug_flags[i])
			seq_printf(m, " %s", x86_bug_flags[i]);
	}

	seq_printf(m, "\nbogomips\t: %lu.%02lu\n",
		   c->loops_per_jiffy/(500000/HZ),
		   (c->loops_per_jiffy/(5000/HZ)) % 100);

#ifdef CONFIG_X86_64
	if (c->x86_tlbsize > 0)
		seq_printf(m, "TLB size\t: %d 4K pages\n", c->x86_tlbsize);
#endif
	seq_printf(m, "clflush size\t: %u\n", c->x86_clflush_size);
	seq_printf(m, "cache_alignment\t: %d\n", c->x86_cache_alignment);
	seq_printf(m, "address sizes\t: %u bits physical, %u bits virtual\n",
		   c->x86_phys_bits, c->x86_virt_bits);

	seq_puts(m, "power management:");
	for (i = 0; i < 32; i++) {
		if (c->x86_power & (1 << i)) {
			if (i < ARRAY_SIZE(x86_power_flags) &&
			    x86_power_flags[i])
				seq_printf(m, "%s%s",
					   x86_power_flags[i][0] ? " " : "",
					   x86_power_flags[i]);
			else
				seq_printf(m, " [%d]", i);
		}
	}

	seq_puts(m, "\n\n");

	return 0;
}

static void calc_nid_vpos(loff_t *pos, unsigned int *pnid, unsigned int *vpos)
{
	int i = 0;

	*pnid = 0;
	*vpos = 0;

	for (i = nr_cpu_ids; i <= num_total_cpus; i++) {
		if ((*pnid) == my_nid)
			(*pnid)++;

		if ((*vpos) == num_cpus[*pnid]) {
			*vpos = 0;
			(*pnid)++;
		}

		if (i == (*pos))
			break;

		(*vpos)++;
	}
}

static void *c_start(struct seq_file *m, loff_t *pos)
{
	unsigned int vpos = 0;
	unsigned int nid = 0;

	if (my_nid == -1)
		goto local;

	if ((*pos) < nr_cpu_ids) {
		goto local;
	} else if ((*pos) == nr_cpu_ids) {
		int i = 0;
		int j = 0;
		bool connected = false;

		/* Check the connection with remote nodes */
		for (i = 0; i < MAX_POPCORN_NODES; i++) {
			if (get_popcorn_node_online(i)) {
				connected = true;
				break;
			}
		}

		if (connected == false) {
			/* No connection */
			goto local;
		} else {
			/* Connection with remote nodes */
			for (i = 0; i < MAX_POPCORN_NODES; i++) {
				if (i == my_nid) {
					num_cpus[i] = nr_cpu_ids;
					j = j + nr_cpu_ids;
					continue;
				}
				if (get_popcorn_node_online(i)) {
					send_remote_cpu_info_request(i);
					num_cpus[i] = get_number_cpus_from_remote_node(i);
					j = j + num_cpus[i];
				} else {
					num_cpus[i] = 0;
				}
			}

			num_total_cpus = j;
			goto remote;
		}
	} else if ((*pos) > nr_cpu_ids) {
		goto remote;
	}

local:
	*pos = cpumask_next(*pos - 1, cpu_online_mask);
	if ((*pos) < nr_cpu_ids) {
		cpu_global_info.remote = 0;
		c = cpu_data(*pos);
		cpu_global_info.c = &c;

		return &cpu_global_info;
	}

	return NULL;

remote:
	if ((*pos) < num_total_cpus) {
		calc_nid_vpos(pos, &nid, &vpos);

		cpu_global_info.remote = 1;
		cpu_global_info.vpos = vpos;
		cpu_global_info.nid = nid;

		return &cpu_global_info;
	}

	return NULL;
}

static void *c_next(struct seq_file *m, void *v, loff_t *pos)
{
	(*pos)++;
	return c_start(m, pos);
}

static void c_stop(struct seq_file *m, void *v)
{
}

static int c_show(struct seq_file *m, void *v)
{
	struct cpu_global_info *cpu_global_info = v;
	struct cpuinfo_x86 *c;

	if (cpu_global_info->remote == 1) {
		remote_proc_cpu_info(m,
			cpu_global_info->nid,
			cpu_global_info->vpos);
	} else {
		c = cpu_global_info->c;
		show_cpuinfo(m, c);
	}

	return 0;
}

const struct seq_operations cpuinfo_op = {
	.start	= c_start,
	.next	= c_next,
	.stop	= c_stop,
	.show	= c_show,
};
