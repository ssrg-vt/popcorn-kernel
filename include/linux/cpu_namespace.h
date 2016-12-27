#ifndef _LINUX_CPU_NS_H
#define _LINUX_CPU_NS_H

#include <linux/sched.h>
#include <linux/nsproxy.h>
#include <linux/kref.h>
#include <linux/ns_common.h>
#include <linux/err.h>

//#include <linux/popcorn_cpuinfo.h>

//content
/* refernce counting */
/* settable max cpus, in order to make pthread happy with the sys_getaffinity  */
/* the chain/of cpumask per connected kernel , basically what akshay did
MAYBE each entry should contain the cpu data of that kernel, like all the cpumasks and nr_cpus etc.
but than refer to a descriptor of that kernel (that will describe the connection) */
/* a cache of descriptors? this is not trivial because it will not be easy to decide which is the maximum size, but if every descriptor has a link to the related kernel, it is not a problem */
/* also another sibling cpuid (in order to support the loading of other cpus */


/* NOTE that the init_cpu_ns should point to the allowed_cpus mask */
//NOOOOOOOOOOOOOOOOOOOOOOOO big error!!!
/* init one deve puntare alle 4 cpumask del sistema!!! present, possible, online, offline */ 
// PROBLEMA some fare con allowed_cpus mask?! (come fanno con i pid?! -> hash mask?!)
struct cpumap {
	atomic_t nr_free; // ?
	void *page; // must be a pointer to cpumask (the local one?!)
	//_remote_cpu_info_response_t * cpus;

	int start;
	int end;
	//holes are accepted
};
#define CPUMAP_ENTRIES    8
//((PID_MAX_LIMIT + 8*PAGE_SIZE - 1)/PAGE_SIZE/8)

struct cpu_namespace;

struct cpubitmap {
  struct list_head next_task;	// linked list of cpubitmaps of the same task
  struct task_struct *task;	// sched set affinity is per task
  struct list_head next_ns;	// linked list of cpubitmaps of the same namespace
  struct cpu_namespace *ns;	// ref namespace
  int size;					// size of bitmap in bytes
  unsigned long bitmap[];	// bitmap (not included here)
};
#define CPUBITMAP_SIZE(cpus) (sizeof(struct cpubitmap) + (BITS_TO_LONGS(cpus) * sizeof(long)))

struct cpu_namespace {
    struct kref kref;  
	struct ns_common ns;
/* WHAT WE NEED, FROM sched_getaffinity/sched_setaffinity */
    int nr_cpu_ids;
    int nr_cpus;
    int cpumask_size; //in bytes
    int _nr_cpumask_bits; //either nr_cpu_ids or nr_cpus
  
    struct cpumask *cpu_online_mask;
			// for init, it is a copy of cpu_online_mask
			// otherwise it is variable length

//  void (*get_online_cpus)(void); // kernel/cpu.c
//  void (*put_online_cpus)(void); // kernel/cpu.c
  
//TODO
//here is the list of cpumask that merge into cpu_online_mask, basically is a list/array of all connected kernels with their starting id
    struct cpumap cpumap[CPUMAP_ENTRIES]; //<-- or a list?! the one of Akshay ?!

//    int last_pid; // we do not need this !!!
//    struct task_struct *child_reaper; //?!?! what is this?!?!
    
    unsigned int level; // level considering to what?! in the pid namespace ?! ok so in this case it is ok!
    struct cpu_namespace *parent; // ok to keep it the ROOT namespace will be cpu_allowed or a copy of it
    
    // TODO we should add a list of users IF we would like to runtime update the namespace
    // PROBLEM: this is something that is not currently supported by glibc/pthread, so we will not work on it now
    
    // TODO add a linked list of cpubitmap allocated
/*#ifdef CONFIG_PROC_FS
    struct vfsmount *proc_mnt;
#endif
#ifdef CONFIG_BSD_PROCESS_ACCT
    struct bsd_acct_struct *bacct;
#endif
    */
};

extern struct cpu_namespace init_cpu_ns;
extern struct cpu_namespace *popcorn_ns;

//TODO must be added in menuconfig & friends
#define CONFIG_CPU_NS
#ifdef CONFIG_CPU_NS

//NOTE
/*
kref counting is ok for cpuns but we should better think about who will handle it
when kref is going to zero is not that the system will eliminate the namespace
because until popcorn is up, i.e. the kernels are connected popcorn is up and running
*/
static inline struct cpu_namespace *get_cpu_ns(struct cpu_namespace *ns)
{
	if (ns != &init_cpu_ns)
		kref_get(&ns->kref);
	return ns;
}

extern struct cpu_namespace *copy_cpu_ns(unsigned long flags, struct cpu_namespace *ns);
extern void free_cpu_ns(struct kref *kref);

static inline void put_cpu_ns(struct cpu_namespace *cpu_ns)
{
	if (cpu_ns != &init_cpu_ns)
		kref_put(&cpu_ns->kref, free_cpu_ns);
}

#else /* CONFIG_CPU_NS */

static inline struct cpu_namespace *get_cpu_ns(struct cpu_namespace *ns)
{
	return ns;
}

static inline struct cpu_namespace *
copy_cpu_ns(unsigned long flags, struct cpu_namespace *ns)
{
	if (flags & CLONE_NEWCPU)
		ns = ERR_PTR(-EINVAL);
	return ns;
}

static inline void put_cpu_ns(struct cpu_namespace *ns)
{
}
#endif /* CONFIG_CPU_NS */

int build_popcorn_ns(int force);
int associate_to_popcorn_ns(struct task_struct * tsk);

#endif /* _LINUX_CPU_NS_H */
