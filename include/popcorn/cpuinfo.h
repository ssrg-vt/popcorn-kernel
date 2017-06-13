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
 *  Akshay Giridhar, SSRG, VirginiaTech
 *  Antonio Barbalace, SSRG, VirginiaTech
 *  Sharath Kumar Bhat, SSRG, VirginiaTech
 *
 */

#ifndef _LINUX_POPCORN_CPUINFO_H
#define _LINUX_POPCORN_CPUINFO_H

#define MAX_ARM_CORES 96
#define MAX_X86_CORES 16

#include <popcorn/bundle.h>

/* For x86_64 cores */
typedef struct __percpu_arch_x86 {
	unsigned int _processor;
	char _vendor_id[16];
	int _cpu_family;
	unsigned int _model;
	char _model_name[64];
	int _stepping;
	unsigned long _microcode;
	unsigned _cpu_freq;
	int _cache_size;
	char _fpu[3];
	char _fpu_exception[3];
	int _cpuid_level;
	char _wp[3];
	char _flags[640];
	unsigned long _nbogomips;
	int _TLB_size;
	unsigned int _clflush_size;
	int _cache_alignment;
	unsigned int _bits_physical;
	unsigned int _bits_virtual;
	char _power_management[64];
} percpu_arch_x86_t;

typedef struct __cpuinfo_arch_x86 {
	int num_cpus;
	percpu_arch_x86_t cpu[MAX_X86_CORES];
} cpuinfo_arch_x86_t;


/* For arm64 cores */
typedef struct __per_core_info_t {
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

typedef struct __cpuinfo_arch_arm64 {
	unsigned int num_cpus;
	per_core_info_t percore[MAX_ARM_CORES];
} cpuinfo_arch_arm64_t;


typedef union __cpuinfo_arch {
	cpuinfo_arch_x86_t x86;
	cpuinfo_arch_arm64_t arm64;
} cpuinfo_arch_t;


struct remote_cpu_info {
	unsigned int _processor;
	enum popcorn_arch arch_type;

	cpuinfo_arch_t arch;
};

/* External function declarations */
extern int fill_cpu_info(struct remote_cpu_info *res);
extern int get_proccessor_id(void);

#endif
