/*
 * File:
 *  cpuinfo.c
 *
 * Description:
 * 	Provides the architecture specific functionality of populating cpuinfo
 *
 * Created on:
 * 	Oct 10, 2014
 *
 * Author:
 *  Akshay Giridhar, SSRG, Virginia Tech
 *  Antonio Barbalace, SSRG, Virginia Tech
 *  Sharath Kumar Bhat, SSRG, Virginia Tech
 *  Sang-Hoon Kim, SSRG, Virginia Tech
 */

#ifndef _LINUX_POPCORN_CPUINFO_H
#define _LINUX_POPCORN_CPUINFO_H

#define MAX_X86_CORES 32

#include <popcorn/bundle.h>
#include <linux/seq_file.h>

struct percore_info_x86 {
	unsigned int processor;
	char vendor_id[16];
	int cpu_family;
	unsigned int model;
	char model_name[64];
	int stepping;
	unsigned long microcode;
	unsigned cpu_freq;
	int cache_size;
	char fpu[3];
	char fpu_exception[3];
	int cpuid_level;
	char wp[3];
	char flags[640];
	unsigned long nbogomips;
	int TLB_size;
	unsigned int clflush_size;
	int cache_alignment;
	unsigned int bits_physical;
	unsigned int bits_virtual;
	char power_management[64];
};

struct cpuinfo_arch_x86 {
	unsigned int num_cpus;
	struct percore_info_x86 cores[MAX_X86_CORES];
};

struct remote_cpu_info {
	enum popcorn_arch arch_type;
	union {
		struct cpuinfo_arch_x86 x86;
	};
};

extern int fill_cpu_info(struct remote_cpu_info *res);
extern void send_remote_cpu_info_request(unsigned int nid);
extern unsigned int get_number_cpus_from_remote_node(unsigned int nid);
extern int remote_proc_cpu_info(struct seq_file *m, unsigned int nid, unsigned int vpos);

#endif // _LINUX_POPCORN_CPUINFO_H
