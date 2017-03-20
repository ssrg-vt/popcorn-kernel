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

// TODO rlist_head should be renamed,
// TODO furthermore a R/W lock must be used to access the list
extern struct list_head rlist_head;

//#include <popcorn/cpuinfo.h>

#define POPCORN_CPUMASK_SIZE 64
#define POPCORN_CPUMASK_BITS (POPCORN_CPUMASK_SIZE * BITS_PER_BYTE)

#if (POPCORN_CPUMASK_BITS < NR_CPUS)
#error POPCORN_CPUMASK_BITS can not be smaller then NR_CPUS
#endif

#define MAX_ARM_CORES 8
#define MAX_X86_CORES 8

enum arch_t {
	arch_unknown = 0,
	arch_x86,
	arch_arm,
};

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
	char _flags[512];
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

typedef struct __per_core_info_t {
	unsigned int processor_id;
	char model_name[64];
	unsigned long cpu_freq;
	char fpu[8];
} per_core_info_t;

typedef struct __cpuinfo_arch_arm64 {
	unsigned int num_cpus;
	char __processor[64];
	per_core_info_t per_core[MAX_ARM_CORES];
	unsigned int cpu_implementer;
	char cpu_arch[16];
	unsigned int cpu_variant;
	unsigned int cpu_part;
	unsigned int cpu_revision;
} cpuinfo_arch_arm64_t;

typedef union __cpuinfo_arch {
	cpuinfo_arch_x86_t x86;
	cpuinfo_arch_arm64_t arm64;
} cpuinfo_arch_t;


struct _remote_cpu_info_data {
	// TODO the following must be added for the messaging layer
	unsigned int endpoint;

	// TODO it must support different cpu type in an heterogeneous setting
	unsigned int _processor;
	enum arch_t arch_type;

	int cpumask_offset;
	int cpumask_size;
	unsigned long cpumask[POPCORN_CPUMASK_SIZE];
	cpuinfo_arch_t arch;
};
typedef struct _remote_cpu_info_data _remote_cpu_info_data_t;

struct _remote_cpu_info_list {
	_remote_cpu_info_data_t _data;
	struct list_head cpu_list_member;
};
typedef struct _remote_cpu_info_list _remote_cpu_info_list_t;


/* External function declarations */
extern int fill_cpu_info(_remote_cpu_info_data_t *res);
extern int get_proccessor_id(void);

#endif
