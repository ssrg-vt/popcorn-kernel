/*
 * File:
  * popcorn_cpuinfo_arch.c
 *
 * Description:
 * 	this file provides the architecture specific functionality of
  * populating cpuinfo
 *
 * Created on:
 * 	Nov 11, 2014
 *
 * Author:
 * 	Ajithchandra Saya, SSRG, VirginiaTech
 *
 */

#include <linux/kernel.h>
//#include <asm/bootparam.h>
//#include <asm/uaccess.h>
#include <linux/mm.h>
#include <asm/setup.h>
#include <linux/slab.h>
#include <linux/highmem.h>
#include <linux/list.h>
#include <linux/smp.h>
#include <linux/cpu.h>
//#include <linux/cpumask.h>
#include <linux/kthread.h>
#include <linux/slab.h>
#include <linux/timex.h>
#include <linux/timer.h>
#include <linux/pcn_kmsg.h>
#include <linux/delay.h>

#include <linux/cpufreq.h>

#include <popcorn/cpuinfo.h>
#include <linux/popcorn_cpuinfo.h>

#include <linux/of_fdt.h>
#include <asm/cputable.h>
#include <asm/cputype.h>

extern const char *machine_name;

int fill_cpu_info(_remote_cpu_info_data_t *res) {

	unsigned int cpu = 0;
	int i, count = 0;
	cpuinfo_arch_arm64_t *arch = &res->arch.arm64;
	static const char *cpu_name;
	struct cpu_info *cpu_info;

	/*
	 * locate processor in the list of supported processor
	 * types.  The linker builds this table for us from the
	 * entries in arch/arm/mm/proc.S
	 */
	cpu_info = lookup_processor_type(read_cpuid_id());
	if (!cpu_info) {
		printk("CPU configuration botched (ID %08x), unable to continue.\n",
		       read_cpuid_id());
		while (1);
	}

	cpu_name = cpu_info->cpu_name;

	res->_processor = cpu;
	res->arch_type = arch_arm;

	strcpy(arch->__processor, cpu_name);

	for_each_online_cpu(i) {
		/*
		 * glibc reads /proc/cpuinfo to determine the number of
		 * online processors, looking for lines beginning with
		 * "processor".  Give glibc what it expects.
		 */
#ifdef CONFIG_SMP
		arch->per_core[i].processor_id = i;
#endif
		strcpy(arch->per_core[i].model_name, machine_name);		

		if (cpu_freq)
			arch->per_core[i].cpu_freq = cpu_freq;

		strcpy(arch->per_core[i].fpu, ((elf_hwcap & HWCAP_FP) ? "yes" : "no"));
		count++;
	}

	arch->num_cpus = count;

	arch->cpu_implementer = read_cpuid_id() >> 24;
	strcpy(arch->cpu_arch, "AArch64");

	arch->cpu_variant = (read_cpuid_id() >> 20) & 15;
	arch->cpu_part = (read_cpuid_id() >> 4) & 0xfff;
	arch->cpu_revision = read_cpuid_id() & 15;

	return 0;
}

int get_proccessor_id(){

	//TODO
	//Not used in popcorn as of now..!!
	return 0;
}
