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
#include <asm/bootparam.h>
#include <asm/uaccess.h>
#include <linux/mm.h>
#include <asm/setup.h>
#include <linux/slab.h>
#include <linux/highmem.h>
#include <linux/list.h>
#include <linux/smp.h>
#include <linux/cpu.h>
//#include <linux/cpumask.h>
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
	cpuinfo_arch_x86_t *arch = &res->arch.x86;

	res->arch_type = POPCORN_ARCH_X86;

	while (count < NR_CPUS) {
		void *p = remote_c_start(&pos);

		if(p == NULL)
			break;

		c = p;
		pos++;

#ifdef CONFIG_SMP
		cpu = c->cpu_index;
#endif

		res->processor = cpu;

		arch->cpu[count].processor = cpu;
		strcpy(arch->cpu[count].vendor_id,
				c->x86_vendor_id[0] ? c->x86_vendor_id : "unknown");
		arch->cpu[count].cpu_family = c->x86;
		arch->cpu[count].model = c->x86_model;
		strcpy(arch->cpu[count].model_name,
				c->x86_model_id[0] ? c->x86_model_id : "unknown");

		if (c->x86_mask || c->cpuid_level >= 0)
			arch->cpu[count].stepping = c->x86_mask;
		else
			arch->cpu[count].stepping = -1;

		if (c->microcode)
			arch->cpu[count].microcode = c->microcode;

		if (cpu_has(c, X86_FEATURE_TSC)) {
			unsigned int freq = cpufreq_quick_get(cpu);

			if (!freq)
				freq = cpu_khz;
			arch->cpu[count].cpu_freq = freq / 1000; //, (freq % 1000);
		}

		/* Cache size */
		if (c->x86_cache_size >= 0)
			arch->cpu[count].cache_size = c->x86_cache_size;

		strcpy(arch->cpu[count].fpu, "yes");
		strcpy(arch->cpu[count].fpu_exception, "yes");
		arch->cpu[count].cpuid_level = c->cpuid_level;
		strcpy(arch->cpu[count].wp, "yes");

		strcpy(arch->cpu[count].flags, "");
		//strcpy(res->_flags,"flags\t\t:");
		for (i = 0; i < 32 * NCAPINTS; i++)
			if (cpu_has(c, i) && x86_cap_flags[i] != NULL){
				strcat(arch->cpu[count].flags, x86_cap_flags[i]);
				strcat(arch->cpu[count].flags, " ");
			}

		arch->cpu[count].nbogomips = c->loops_per_jiffy / (500000 / HZ);
		//(c->loops_per_jiffy/(5000/HZ)) % 100);

#ifdef CONFIG_X86_64
		if (c->x86_tlbsize > 0)
			arch->cpu[count].TLB_size = c->x86_tlbsize;
#endif
		arch->cpu[count].clflush_size = c->x86_clflush_size;
		arch->cpu[count].cache_alignment = c->x86_cache_alignment;
		arch->cpu[count].bits_physical = c->x86_phys_bits;
		arch->cpu[count].bits_virtual = c->x86_virt_bits;

		strcpy(arch->cpu[count].power_management, "");
		for (i = 0; i < 32; i++) {
			if (c->x86_power & (1 << i)) {
				if (i < ARRAY_SIZE(x86_power_flags) && x86_power_flags[i])
					strcat(arch->cpu[count].flags,
							x86_power_flags[i][0] ? " " : "");
			}
		}

		count++;
		arch->num_cpus = count;
		//printk("Number of cpus = %d\n", arch->num_cpus);
	}

	return 0;
}

int get_proccessor_id()
{
	unsigned int a, b, feat;

	asm volatile(
			 "cpuid"							// call cpuid
			 : "=a" (a), "=b" (b), "=d" (feat)	// outputs
			 : "0" (1)							// inputs
			 : "cx" );

	return !(feat & (1 << 25));
}
