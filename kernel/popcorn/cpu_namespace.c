
/*
 * Cpu namespaces
 *
 * (C) 2014 Antonio Barbalace, antoniob@vt.edu, SSRG VT
 */

#include <linux/cpumask.h>
#include <linux/syscalls.h>
#include <linux/err.h>
#include <linux/acct.h>
#include <linux/slab.h>
#include <linux/proc_fs.h>
#include <linux/proc_ns.h>
#include <linux/dcache.h>
#include <linux/file.h>
#include <linux/module.h>
#include <linux/namei.h>

#include <linux/cpu_namespace.h>

static DEFINE_MUTEX(cpu_caches_mutex);
static struct kmem_cache *cpu_ns_cachep;

struct cpu_namespace init_cpu_ns = {
	.kref = {
		.refcount = ATOMIC_INIT(2),
	},
	.ns = {
		.inum = PROC_CPU_INIT_INO,
		.ops = &cpuns_operations,
	},
	.nr_cpus = NR_CPUS,
	//.cpumask_size = (BITS_TO_LONGS(NR_CPUS) * sizeof(long)),
	//.cpu_online_mask = cpu_online_mask,
	//.get_online_cpus = get_online_cpus;
	//.get_offline_cpus = get_offline_cpus,
	.parent = NULL,
	.level = 0,
};
EXPORT_SYMBOL_GPL(init_cpu_ns);

struct cpu_namespace *popcorn_ns = NULL;

static struct cpu_namespace *create_cpu_namespace(struct cpu_namespace *parent_cpu_ns)
{
	struct cpu_namespace *ns = kmem_cache_zalloc(cpu_ns_cachep, GFP_KERNEL);
	if (!ns) return ERR_PTR(-ENOMEM);

	kref_init(&ns->kref);
	ns_alloc_inum(&ns->ns);
	ns->parent = get_cpu_ns(parent_cpu_ns);
	ns->level = ns->parent->level + 1;
	ns->ns = ns->parent->ns;
	// ns->nr_cpu_ids = ns->parent->nr_cpu_ids;
	// ns->nr_cpus = ns->parent->nr_cpus;
	// ns->cpumask_size = ns->parent->cpumask_size;
	// _nr_cpumask_bits ?
	// ns->cpu_online_mask = ns->parent->cpu_online_mask;

	return ns;
}

static void destroy_cpu_namespace(struct cpu_namespace *ns)
{
	kmem_cache_free(cpu_ns_cachep, ns);
}

struct cpu_namespace *copy_cpu_ns(unsigned long flags, struct cpu_namespace *old_ns)
{
	if (!(flags & CLONE_NEWCPU))
		return get_cpu_ns(old_ns);

	if (flags & (CLONE_THREAD|CLONE_PARENT)) {
		printk("%s: grande cacca\n", __func__);
		dump_stack();
		return ERR_PTR(-EINVAL);
	}
	return create_cpu_namespace(old_ns);
}

void free_cpu_ns(struct kref *kref)
{
	struct cpu_namespace *ns, *parent;

	ns = container_of(kref, struct cpu_namespace, kref);

	parent = ns->parent;
	destroy_cpu_namespace(ns);

	if (parent != NULL)
		put_cpu_ns(parent);
}

static struct cpu_namespace *to_cpu_ns(struct ns_common *ns)
{
	return container_of(ns, struct cpu_namespace, ns);
}

static struct ns_common *cpuns_get(struct task_struct *task)
{
	struct ns_common *ns = NULL;
	struct nsproxy *nsproxy;

	task_lock(task);
	nsproxy = task->nsproxy;
	if (nsproxy) {
		get_cpu_ns(nsproxy->cpu_ns);
		ns = &nsproxy->cpu_ns->ns;
	}
	task_unlock(task);

	return ns;
}

static void cpuns_put(struct ns_common *ns)
{
	return put_cpu_ns(to_cpu_ns(ns));
}

static int cpuns_install(struct nsproxy *nsproxy, struct ns_common *ns)
{
	/* Ditch state from the old cpu namespace */
	//exit_sem(current); //from ipc/namespace.c

	// from kernel/cpu_namespace.c
	/*        struct cpu_namespace *active = task_active_pid_ns(current);//is ns_of_pid(task_pid(tsk))
		  struct cpu_namespace *ancestor, *new = ns;

		  if (!capable(CAP_SYS_ADMIN))
		  return -EPERM;

		  if (new->level < active->level)
		  return -EINVAL;

		  ancestor = new;
		  while (ancestor->level > active->level)
		  ancestor = ancestor->parent;
		  if (ancestor != active)
		  return -EINVAL;
	 */
	put_cpu_ns(nsproxy->cpu_ns);
	nsproxy->cpu_ns = get_cpu_ns(to_cpu_ns(ns));
	return 0;
}

const struct proc_ns_operations cpuns_operations = {
	.name           = "cpu",
	.type           = CLONE_NEWCPU,
	.get            = cpuns_get,
	.put            = cpuns_put,
	.install        = cpuns_install,
};

/*****************************************************************************/

// INSTEAD of creating a file we will do attach to popcorn syscall or WRITE on /proc/popcorn
// Il seguente codice va probabilmente spostato in popcorn
// -> The following code should probably be moved to popcorn


static int read_notify_cpu_ns(struct seq_file *file, void *ptr)
{
	struct cpu_namespace *ns = current->nsproxy->cpu_ns;

	seq_printf(file, "cpu_ns for task %s(0x%p)\n"
			"popcorn_ns 0x%p, ns 0x%p, level %d, parent 0x%p\n",
			current->comm, current,
			popcorn_ns, ns, ns->level, ns->parent);
	return 0;
}

// TODO move to popcorn.c/mklinux.c/kinit.c
// the following is temporary and should be updated in a final version, this should be known in the process_server.c --- this shuold be moved in something like kinit.c
#if 0 // beowulf: cpumask is removed for rack-popcorn
extern unsigned int offset_cpus; //from kernel/smp.c
#endif

/*
 * This function should be called every time a new kernel will join the popcorn
 * namespace. Note that if there are applications using the popcorn namespace
 * it is not possible to modify the namespace.
 * TODO: "force" updates the * namespace data forcefully.
 */
// NOTE this will modify the global variable popcorn_ns (so, no need to pass in anything)
int __init popcorn_ns_init(int force)
{
	if (popcorn_ns) return -EEXIST;

#if 0 // beowulf
	int cnr_cpu_ids = 0;

	_remote_cpu_info_list_t *r;
	// struct cpumask *pcpum = 0;
	unsigned long *summary, *tmp;
	int size, offset;
	int cpuid;

	//    if (kref says that no one is using it continue) // TODO
	//        return -EBUSY;

	// TODO lock the list of kernels

	//-------------------------------- SIZE --------------------------------
	/* calculate the minimum size of the bitmask */
	// current kernel
	cnr_cpu_ids += cpumask_weight(cpu_online_mask); // or nr_cpu_ids
	// other kernels
	cpuid = -1;
	list_for_each_entry(r, &rlist_head, cpu_list_member) {
		pcpum = (struct cpumask *)&(r->_data.cpumask); //&(r->_data._cpumask);
		cnr_cpu_ids += bitmap_weight(cpumask_bits(pcpum),
				(r->_data.cpumask_size * BITS_PER_BYTE));//cpumask_weight(pcpum);
	}

	//--------------------------- GENERATE THE MASK ----------------------------
	size = max(cpumask_size(), (BITS_TO_LONGS(cnr_cpu_ids) * sizeof(long)));
	summary = kmalloc(size, GFP_KERNEL);
	if (!summary) {
		printk(KERN_ERR"%s: cannot allocate summary\n", __func__);
		return -ENOMEM;
	}
	tmp = kmalloc(size, GFP_KERNEL);
	if (!tmp) {
		kfree(summary);
		printk(KERN_ERR"%s: cannot allocate tmp\n", __func__);
		return -ENOMEM;
	}
	//printk(KERN_ERR"%s: cnr_cpu_ids: %d size:%d summary %p tmp %p\n",
	//	__func__, cnr_cpu_ids, size, summary, tmp);
	// current kernel
	bitmap_zero(summary, size * BITS_PER_BYTE);
	bitmap_copy(summary, cpumask_bits(cpu_online_mask), nr_cpu_ids); // NOTE that the current cpumask is not included in the remote list
	bitmap_shift_left(summary, summary, offset_cpus, cnr_cpu_ids);
	// other kernels
	cpuid = -1;
	list_for_each_entry(r, &rlist_head, cpu_list_member) {
		cpuid = r->_data._processor;
		pcpum = to_cpumask(r->_data.cpumask);//&(r->_data._cpumask);
		offset = r->_data.cpumask_offset;

		//	printk("%s, cpumask_offset %d\n",__func__,offset);
		//TODO we should update kinit.c in order to support variable length cpumask

		bitmap_zero(tmp, size * BITS_PER_BYTE);
		bitmap_copy(tmp, cpumask_bits(pcpum), (r->_data.cpumask_size *BITS_PER_BYTE));
		bitmap_shift_left(tmp, tmp, offset, cnr_cpu_ids);
		bitmap_or(summary, summary, tmp, cnr_cpu_ids);
	}
#endif

	//------------------------ GENERATE the namespace ------------------------

	//TODO lock the namespace
	popcorn_ns = create_cpu_namespace(&init_cpu_ns);
	if (IS_ERR(popcorn_ns)) {
		/*
		kfree(summary);
		kfree(tmp);
		*/
		return -ENODEV;
		//TODO release list lock
	}
	// tmp_ns->cpu_online_mask = 0;


	/*
	if (popcorn_ns->cpu_online_mask) {
		if (popcorn_ns->cpu_online_mask != cpu_online_mask)
			kfree(popcorn_ns->cpu_online_mask);
		else
			printk(KERN_ERR"%s: there is something weird, popcorn was associated with cpu_online_mask\n", __func__);
	}
	popcorn_ns->cpu_online_mask = to_cpumask(summary);
	*/

	/*
	popcorn_ns->nr_cpus = cnr_cpu_ids; // the followings are intentional
	popcorn_ns->nr_cpu_ids = cnr_cpu_ids;
	popcorn_ns->_nr_cpumask_bits = cnr_cpu_ids;
	popcorn_ns->cpumask_size = BITS_TO_LONGS(cnr_cpu_ids) * sizeof(long);
	*/

	// TODO maybe the following should be different
	// the idea is that if does not have parent, parent should be init_cpu_ns)
	/*
	if (!popcorn_ns->parent) {
		popcorn_ns->parent = &init_cpu_ns;
		popcorn_ns->level = 1;
	}
	*/

	//TODO unlock popcorn namespace
	//TODO unlock kernel list
	//create_thread_pull();
	return 0;
}


#if 0
/* despite this is not the correct way to go, this is working in this way
 * every time we are writing something on this file (even NULL)
 * we are rebuilding a new popcorn namespace merging all the available kernels
 */
static ssize_t write_notify_cpu_ns(struct file *file, const char __user *buffer, unsigned long count, loff_t *data)
{
	int ret;

	printk("%s, entered in write proc popcorn\n",__func__);
	get_task_struct(current);

#undef USE_OLD_IMPLEM
#ifdef USE_OLD_IMPLEM
	int cnr_cpu_ids, cnr_cpus;
	struct cpu_namespace *ns;

	ns = current->nsproxy->cpu_ns;

	// TODO convert everything in bitmap (cpumask are fixed size bitmaps
	_remote_cpu_info_list_t *r;
	struct cpumask *pcpum =0;
	struct cpumask tmp;
	struct cpumask * summary= kmalloc(cpumask_size(), GFP_KERNEL);
	if (!summary) {
		printk("kmalloc returned 0?!");
	}
	cnr_cpu_ids += cpumask_weight(cpu_online_mask); // or nr_cpu_ids
	cpumask_copy(summary, cpu_online_mask); // NOTE that the current cpumask is not included in the remote list

	int cpuid =-1;
	list_for_each(r, &rlist_head, cpu_list_member) {
		cpuid = r->_data._processor;
		pcpum = &(r->_data._cpumask);

		// TODO use offsetting with cpuid
		cpumask_copy(&tmp, summary);
		cpumask_or (summary, &tmp, pcpum);

		cnr_cpu_ids += cpumask_weight(pcpum);
	}
	printk("%s, after list for each of cpus\n",__func__);
	//associate the new cpu mask with the namespace
	ns->cpu_online_mask = summary;
	ns->nr_cpus = NR_CPUS;
	ns->nr_cpu_ids = cnr_cpu_ids;
#else
	// the new code builds a popcorn namespace and we should remove the ref count and destroy the current (if is not init) and attach to popcorn..

	/* if the namespace does not exist, create it */
	if (!popcorn_ns) {
		printk(KERN_ERR"%s: popcorn is null now!\n", __func__);
		if ((ret = popcorn_ns_init(0))) {
			printk(KERN_ERR"%s: build_popcorn returned: %d\n", __func__, ret);
			return count;
		}
	}

	/* if we are already attached, let's skip the unlinking and linking */
	if (current->nsproxy->cpu_ns != popcorn_ns) {
		put_cpu_ns(current->nsproxy->cpu_ns);
		current->nsproxy->cpu_ns = get_cpu_ns(popcorn_ns);
		printk(KERN_ERR"%s: cpu_ns %p\n", __func__, current->nsproxy->cpu_ns);
	} else {
		printk(KERN_ERR"%s: already attached to popcorn(%p)\n",
				__func__, popcorn_ns);
	}
#endif

	// -------------- UPDATE cpus_allowed map -----------------
#ifdef USE_OLD_IMPLEM
	if (current->cpus_allowed_map == NULL) {// in this case I have to convert allowed to global mask
		int size = CPUBITMAP_SIZE(ns->nr_cpu_ids);
		struct cpubitmap * cbitm = kmalloc(size, GFP_KERNEL);// here we should use  a cache instead of mkalloc
		BUG_ON(!cbitm);

		cbitm->size = size;
		cbitm->ns = ns;
		bitmap_zero(cbitm->bitmap, ns->nr_cpu_ids);
		bitmap_copy(cbitm->bitmap, cpumask_bits(&current->cpus_allowed), cpumask_size());
		bitmap_shift_left(cbitm->bitmap, cbitm->bitmap, offset_cpus, ns->nr_cpu_ids);
		current->cpus_allowed_map = cbitm;
	}
	// TODO support the else case
#else
	if ((ret = associate_to_popcorn_ns(current)))  {
		printk(KERN_ERR"associate_to_popcorn_ns returned: %d\n", ret);
		return count;
	}
#endif

	printk("task %p %s associated with popcorn (local nr_cpu_ids %d NR_CPUS %d cpumask_bits %d\n", current, current->comm, nr_cpu_ids, NR_CPUS, nr_cpumask_bits);

	put_task_struct(current);

	printk("%s, after put_task_struct\n",__func__);
	return count;
}
#endif

int popcorn_ns_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, read_notify_cpu_ns, NULL);
}

static struct file_operations fops = {
	.open = popcorn_ns_proc_open,
	.read = seq_read,
//	.write = write_notify_cpu_ns,
	.release = single_release,
};

int register_popcorn_ns(void)
{
	struct proc_dir_entry *res;

	printk(KERN_INFO"Creating popcorn namespace entry in procfs\n");

	res = proc_create("cpu_ns", S_IRUGO, NULL, &fops);
	if (!res) {
		printk(KERN_ERR"Popcornlinux NS: (ERROR) Failed to create proc entry!");
		return -1;
	}
	return 0;
}

/*
 * TODO: Need to find an alternate implementation since macros like PROC_I
 * are no longer publically available in 3.12, the definitions are
 * moved to private header file inside proc/fs/internal.h
 */
#if 0

/// initial idea for attaching a process to popcorn namespace
/* This function should be called everytime a new kernel will be registered
 * in /kernel/kinit.c . When the number of kernels in the list is greater than
 * 1 the /proc/popcorn entry will be created.
 */
int notify_cpu_ns(void)
{
	// lock the cpu namespace

	// count the number of elements and calculate the cpumask/bitmap that is required to contain them
	// min size is cpumask

	// allocate cpumask and populate it

	// finish namespace intialization

	//unlock cpu namespace
	/*
	// if kernels > 1 then create /proc/popcorn
	struct proc_dir_entry *res; // TODO mettiamola globale
	res = create_proc_entry("popcorn_ext", S_IRUGO, NULL);
	if (!res) {
	printk(KERN_ALERT"%s: create_proc_entry failed (%p)\n", __func__, res);
	return -ENOMEM;
	}
	res->read_proc = read_notify_cpu_ns;
	//	res->write_proc = write_notify_cpu_ns;
	res->proc_fops  = &ns_file_operations; // required by setns

	// alternative, when we open the file andiamo a settare i PROC_I elements
	 */
	// the following is probably not correct

	struct dentry *dentry = NULL;
	struct inode *dir = NULL;
	struct proc_inode *ei = NULL;
	struct path old_path; int err = 0;

	err = kern_path("/proc/", LOOKUP_FOLLOW, &old_path);
	if (err)
		return err;

	dentry = old_path.dentry;
	dir = dentry->d_inode * inode;

	// proc_pid_make_inode
	inode = new_inode(dir->i_sb);
	if (!inode)
		return err;

	//ei = PROC_I(inode);
	inode->i_ino = get_next_ino();
	inode->i_mtime = inode->i_atime = inode->i_ctime = CURRENT_TIME;
	inode->i_op = dir->i_op; //&proc_def_inode_operations;


	/*	ns = ns_ops->get(task);
		if (!ns)
		goto out_iput;*/

	//ei = PROC_I(inode);
	inode->i_mode = S_IFREG|S_IRUSR;

	extern const struct file_operations ns_file_operations;
	inode->i_fop  = &ns_file_operations;
	ei->ns_ops    = &cpuns_operations;
	ei->ns	      = &popcorn_ns;

	//	d_set_d_op(dentry, &pid_dentry_operations); //this are already set in /proc
	d_add(dentry, inode);

	// TODO add the real dentry entry! with the name of the file!

	return 0;
}
#endif


static __init int cpu_namespaces_init(void)
{
	printk("Initializing cpu_namespace\n");

	// init_cpu_ns.cpu_online_mask = (struct cpumask *)cpu_online_mask;

	cpu_ns_cachep = KMEM_CACHE(cpu_namespace, SLAB_PANIC);
	if (!cpu_ns_cachep)
		printk("%s: ERROR KMEM_CACHE\n", __func__);

#ifdef CONFIG_POPCORN
	return register_popcorn_ns();
#else
	return 0;
#endif
}

__initcall(cpu_namespaces_init);

/* the idea is indeed to do similarly to net, i.e. there will be a file
   somewhere and can be in /proc/popcorn or /var/run/cpuns/possible_configurations
   in which you can do setns and being part of the namespace */
