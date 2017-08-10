/*
 * File:
 *  cpuinfo.c
 *
 * Description:
 * 	This file provides the architecture specific functionality of
 * populating cpuinfo
 *
 * Created on:
 * 	Oct 10, 2014
 *
 * Author:
 *  Akshay Giridhar, SSRG, VirginiaTech
 *  Antonio Barbalace, SSRG, VirginiaTech
 *  Sharath Kumar Bhat, SSRG, VirginiaTech
 *
 */

#ifndef _LINUX_POPCORN_CPUINFO_H
#define _LINUX_POPCORN_CPUINFO_H

#define MAX_ARM_CORES 96
#define MAX_X86_CORES 32

#include <popcorn/bundle.h>

/* For x86_64 cores */
typedef struct percpu_arch_x86 {
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
} percpu_arch_x86_t;

typedef struct cpuinfo_arch_x86 {
	int num_cpus;
	percpu_arch_x86_t cpu[MAX_X86_CORES];
} cpuinfo_arch_x86_t;


/* For arm64 cores */
typedef struct per_core_info_t {
	unsigned int processor_id;
	bool compat;
	char model_name[64];
	int model_rev;
	char model_elf[8];
	unsigned long bogo_mips;
	unsigned long bogo_mips_fraction;
	char flags[64];
	unsigned int cpu_implementer;
	unsigned int cpu_archtecture;
	unsigned int cpu_variant;
	unsigned int cpu_part;
	unsigned int cpu_revision;
} per_core_info_t;

typedef struct cpuinfo_arch_arm64 {
	unsigned int num_cpus;
	per_core_info_t percore[MAX_ARM_CORES];
} cpuinfo_arch_arm64_t;


typedef union cpuinfo_arch {
	cpuinfo_arch_x86_t x86;
	cpuinfo_arch_arm64_t arm64;
} cpuinfo_arch_t;


struct remote_cpu_info {
	unsigned int processor;
	enum popcorn_arch arch_type;

	cpuinfo_arch_t arch;
};

/* External function declarations */
extern int fill_cpu_info(struct remote_cpu_info *res);
extern int get_proccessor_id(void);

#endif
