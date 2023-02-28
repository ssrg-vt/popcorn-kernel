/*
 * File:
  * popcorn_cpuinfo_arch.c
 *
 * Description:
 * 	this file provides the architecture specific functionality of
  * populating cpuinfo
 *
 * Created on:
 * 	Oct 10, 2014
 *
 * Author:
 * 	Sharath Kumar Bhat, SSRG, VirginiaTech
 *
 */
#include <linux/kernel.h>
#include <linux/smp.h>
#include <linux/cpu.h>
#include <linux/slab.h>
#include <linux/timex.h>
#include <linux/timer.h>
#include <linux/delay.h>

#include <linux/cpufreq.h>

#include <popcorn/cpuinfo.h>

static void *remote_c_start(loff_t *pos)
{
	if (*pos == 0) /* just in case, cpu 0 is not the first */
		*pos = cpumask_first(cpu_online_mask);
	else {
		*pos = cpumask_next(*pos - 1, cpu_online_mask);
	}

	if ((*pos) < nr_cpu_ids)
		return &cpu_data(*pos);
	return NULL;
}

int fill_cpu_info(struct remote_cpu_info *res)
{
	loff_t pos = 0;
	struct cpuinfo_x86 *c;
	unsigned int cpu = 0;
	int i, count = 0;
	struct cpuinfo_arch_x86 *arch = &res->x86;

	res->arch_type = POPCORN_ARCH_X86;

	while (count < NR_CPUS) {
		void *p = remote_c_start(&pos);
		struct percore_info_x86 *core = &arch->cores[count];

		if(p == NULL)
			break;

		c = p;
		pos++;

#ifdef CONFIG_SMP
		cpu = c->cpu_index;
#endif
		core->processor = cpu;
		strcpy(core->vendor_id,
				c->x86_vendor_id[0] ? c->x86_vendor_id : "unknown");
		core->cpu_family = c->x86;
		core->model = c->x86_model;
		strcpy(core->model_name,
				c->x86_model_id[0] ? c->x86_model_id : "unknown");

		if (c->x86_model || c->cpuid_level >= 0)
			core->stepping = c->x86_model;
		else
			core->stepping = -1;

		if (c->microcode)
			core->microcode = c->microcode;

		if (cpu_has(c, X86_FEATURE_TSC)) {
			unsigned int freq = cpufreq_quick_get(cpu);

			if (!freq)
				freq = cpu_khz;
			core->cpu_freq = freq / 1000;
		}

		/* Cache size */
		if (c->x86_cache_size >= 0)
			core->cache_size = c->x86_cache_size;

		strcpy(core->fpu, "yes");
		strcpy(core->fpu_exception, "yes");
		core->cpuid_level = c->cpuid_level;
		strcpy(core->wp, "yes");

		strcpy(core->flags, "");
		for (i = 0; i < 32 * NCAPINTS; i++)
			if (cpu_has(c, i) && x86_cap_flags[i] != NULL){
				strcat(core->flags, x86_cap_flags[i]);
				strcat(core->flags, " ");
			}

		core->nbogomips = c->loops_per_jiffy / (500000 / HZ);

#ifdef CONFIG_X86_64
		if (c->x86_tlbsize > 0)
			core->TLB_size = c->x86_tlbsize;
#endif
		core->clflush_size = c->x86_clflush_size;
		core->cache_alignment = c->x86_cache_alignment;
		core->bits_physical = c->x86_phys_bits;
		core->bits_virtual = c->x86_virt_bits;

		strcpy(core->power_management, "");
		for (i = 0; i < 32; i++) {
			if (c->x86_power & (1 << i)) {
				if (i < ARRAY_SIZE(x86_power_flags) && x86_power_flags[i])
					strcat(core->flags,
							x86_power_flags[i][0] ? " " : "");
			}
		}

		count++;
	}
	arch->num_cpus = count;

	return 0;
}

int get_proccessor_id(void)
{
	unsigned int a, b, feat;

	asm volatile(
			 "cpuid"							// call cpuid
			 : "=a" (a), "=b" (b), "=d" (feat)	// outputs
			 : "0" (1)							// inputs
			 : "cx" );

	return !(feat & (1 << 25));
}
