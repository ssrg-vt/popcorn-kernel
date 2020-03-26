// SPDX-License-Identifier: GPL-2.0
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/hugetlb.h>
#include <linux/mman.h>
#include <linux/mmzone.h>
#include <linux/proc_fs.h>
#include <linux/percpu.h>
#include <linux/quicklist.h>
#include <linux/seq_file.h>
#include <linux/swap.h>
#include <linux/vmstat.h>
#include <linux/atomic.h>
#include <linux/vmalloc.h>
#ifdef CONFIG_CMA
#include <linux/cma.h>
#endif
#include <asm/page.h>
#include <asm/pgtable.h>
#include "internal.h"
#ifdef CONFIG_POPCORN_REMOTE_INFO
#include <popcorn/remote_meminfo.h>
#endif

void __attribute__((weak)) arch_report_meminfo(struct seq_file *m)
{
}

static void show_val_kb(struct seq_file *m, const char *s, unsigned long num)
{
	seq_put_decimal_ull_width(m, s, num << (PAGE_SHIFT - 10), 8);
	seq_write(m, " kB\n", 4);
}

static int meminfo_proc_show(struct seq_file *m, void *v)
{
	struct sysinfo i;
	unsigned long committed;
	long cached;
	long available;
#ifdef CONFIG_POPCORN_REMOTE_INFO
	remote_mem_info_response_t rem_mem;
#endif
	unsigned long pages[NR_LRU_LISTS];
	unsigned long sreclaimable, sunreclaim;
	int lru;

	si_meminfo(&i);
	si_swapinfo(&i);
	committed = percpu_counter_read_positive(&vm_committed_as);

	cached = global_node_page_state(NR_FILE_PAGES) -
			total_swapcache_pages() - i.bufferram;
	if (cached < 0)
		cached = 0;

	for (lru = LRU_BASE; lru < NR_LRU_LISTS; lru++)
		pages[lru] = global_node_page_state(NR_LRU_BASE + lru);

	available = si_mem_available();
	sreclaimable = global_node_page_state(NR_SLAB_RECLAIMABLE);
	sunreclaim = global_node_page_state(NR_SLAB_UNRECLAIMABLE);

#ifdef CONFIG_POPCORN_REMOTE_INFO
	show_val_kb(m, "MemTotal:       ", i.totalram + rem_mem.MemTotal);
	show_val_kb(m, "MemFree:        ", i.freeram + rem_mem.MemFree);
	show_val_kb(m, "MemAvailable:   ", available + rem_mem.MemAvailable);
	show_val_kb(m, "Buffers:        ", i.bufferram + rem_mem.Buffers);
	show_val_kb(m, "Cached:         ", cached + rem_mem.Cached);
	show_val_kb(m, "SwapCached:     ", total_swapcache_pages() + rem_mem.SwapCached);
	show_val_kb(m, "Active:         ", pages[LRU_ACTIVE_ANON] +
					   pages[LRU_ACTIVE_FILE] +
					   rem_mem.Active);
	show_val_kb(m, "Inactive:       ", pages[LRU_INACTIVE_ANON] +
					   pages[LRU_INACTIVE_FILE] +
					   rem_mem.Inactive);
	show_val_kb(m, "Active(anon):   ", pages[LRU_ACTIVE_ANON] +
					   rem_mem.Active_anon);
	show_val_kb(m, "Inactive(anon): ", pages[LRU_INACTIVE_ANON] +
					   rem_mem.Inactive_anon);
	show_val_kb(m, "Active(file):   ", pages[LRU_ACTIVE_FILE] +
					   rem_mem.Active_file);
	show_val_kb(m, "Inactive(file): ", pages[LRU_INACTIVE_FILE] +
					   rem_mem.Inactive_file);
	show_val_kb(m, "Unevictable:    ", pages[LRU_UNEVICTABLE] +
					   rem_mem.Unevictable);
	show_val_kb(m, "Mlocked:        ", global_zone_page_state(NR_MLOCK) + rem_mem.Mlocked);

#ifdef CONFIG_HIGHMEM
	show_val_kb(m, "HighTotal:      ", i.totalhigh + rem_mem.HighTotal);
	show_val_kb(m, "HighFree:       ", i.freehigh + rem_mem.HighFree);
	show_val_kb(m, "LowTotal:       ", (i.totalram - i.totalhigh) + rem_mem.LowTotal);
	show_val_kb(m, "LowFree:        ", (i.freeram - i.freehigh) + rem_mem.LowFree);
#endif
#ifndef CONFIG_MMU
	show_val_kb(m, "MmapCopy:       ",
		    (unsigned long)atomic_long_read(&mmap_pages_allocated) + rem_mem.MmapCopy);
#endif

	show_val_kb(m, "SwapTotal:      ", i.totalswap + rem_mem.SwapTotal);
	show_val_kb(m, "SwapFree:       ", i.freeswap + rem_mem.SwapFree);
	show_val_kb(m, "Dirty:          ",
		    global_node_page_state(NR_FILE_DIRTY) + rem_mem.Dirty);
	show_val_kb(m, "Writeback:      ",
		    global_node_page_state(NR_WRITEBACK) + rem_mem.Writeback);
	show_val_kb(m, "AnonPages:      ",
		    global_node_page_state(NR_ANON_MAPPED) + rem_mem.AnonPages);
	show_val_kb(m, "Mapped:         ",
		    global_node_page_state(NR_FILE_MAPPED) + rem_mem.Mapped);
	show_val_kb(m, "Shmem:          ", i.sharedram + rem_mem.Shmem);
	show_val_kb(m, "Slab:           ",
		    global_node_page_state(NR_SLAB_RECLAIMABLE) +
		    global_node_page_state(NR_SLAB_UNRECLAIMABLE) +
		    rem_mem.Slab);

	show_val_kb(m, "SReclaimable:   ",
		    global_node_page_state(NR_SLAB_RECLAIMABLE) +
		    rem_mem.SReclaimable);
	show_val_kb(m, "SUnreclaim:     ",
		    global_node_page_state(NR_SLAB_UNRECLAIMABLE) +
		    rem_mem.SUnreclaim);
	seq_printf(m, "KernelStack:    %8lu kB\n",
		   global_zone_page_state(NR_KERNEL_STACK_KB) +
		   rem_mem.KernelStack);
	show_val_kb(m, "PageTables:     ",
		    global_zone_page_state(NR_PAGETABLE) +
		    rem_mem.PageTables);
#ifdef CONFIG_QUICKLIST
	show_val_kb(m, "Quicklists:     ",
		    quicklist_total_size() +
		    rem_mem.Quicklists);
#endif

	show_val_kb(m, "NFS_Unstable:   ",
		    global_node_page_state(NR_UNSTABLE_NFS) +
		    rem_mem.NFS_Unstable);
	show_val_kb(m, "Bounce:         ",
		    global_zone_page_state(NR_BOUNCE) +
		    rem_mem.Bounce);
	show_val_kb(m, "WritebackTmp:   ",
		    global_node_page_state(NR_WRITEBACK_TEMP) +
		    rem_mem.WritebackTmp);
	show_val_kb(m, "CommitLimit:    ", vm_commit_limit() + rem_mem.CommitLimit);
	show_val_kb(m, "Committed_AS:   ", committed + rem_mem.Committed_AS);
	seq_printf(m, "VmallocTotal:   %8lu kB\n",
		   (unsigned long)VMALLOC_TOTAL >> 10 +
		   rem_mem.VmallocTotal);
	show_val_kb(m, "VmallocUsed:    ", 0ul);
	show_val_kb(m, "VmallocChunk:   ", 0ul);
	show_val_kb(m, "Percpu:         ", pcpu_nr_pages());

#ifdef CONFIG_MEMORY_FAILURE
	seq_printf(m, "HardwareCorrupted: %5lu kB\n",
		   atomic_long_read(&num_poisoned_pages) << (PAGE_SHIFT - 10) +
		   rem_mem.HardwareCorrupted);
#endif

#ifdef CONFIG_TRANSPARENT_HUGEPAGE
	show_val_kb(m, "AnonHugePages:  ",
		    global_node_page_state(NR_ANON_THPS) * HPAGE_PMD_NR +
		    rem_mem.AnonHugePages);
	show_val_kb(m, "ShmemHugePages: ",
		    global_node_page_state(NR_SHMEM_THPS) * HPAGE_PMD_NR);
	show_val_kb(m, "ShmemPmdMapped: ",
		    global_node_page_state(NR_SHMEM_PMDMAPPED) * HPAGE_PMD_NR);
#endif

#ifdef CONFIG_CMA
	show_val_kb(m, "CmaTotal:       ", totalcma_pages + rem_mem.CmaTotal);
	show_val_kb(m, "CmaFree:        ",
		    global_zone_page_state(NR_FREE_CMA_PAGES) + rem_mem.CmaFree);
#endif
#else
	show_val_kb(m, "MemTotal:       ", i.totalram);
	show_val_kb(m, "MemFree:        ", i.freeram);
	show_val_kb(m, "MemAvailable:   ", available);
	show_val_kb(m, "Buffers:        ", i.bufferram);
	show_val_kb(m, "Cached:         ", cached);
	show_val_kb(m, "SwapCached:     ", total_swapcache_pages());
	show_val_kb(m, "Active:         ", pages[LRU_ACTIVE_ANON] +
					   pages[LRU_ACTIVE_FILE]);
	show_val_kb(m, "Inactive:       ", pages[LRU_INACTIVE_ANON] +
					   pages[LRU_INACTIVE_FILE]);
	show_val_kb(m, "Active(anon):   ", pages[LRU_ACTIVE_ANON]);
	show_val_kb(m, "Inactive(anon): ", pages[LRU_INACTIVE_ANON]);
	show_val_kb(m, "Active(file):   ", pages[LRU_ACTIVE_FILE]);
	show_val_kb(m, "Inactive(file): ", pages[LRU_INACTIVE_FILE]);
	show_val_kb(m, "Unevictable:    ", pages[LRU_UNEVICTABLE]);
	show_val_kb(m, "Mlocked:        ", global_zone_page_state(NR_MLOCK));

#ifdef CONFIG_HIGHMEM
	show_val_kb(m, "HighTotal:      ", i.totalhigh);
	show_val_kb(m, "HighFree:       ", i.freehigh);
	show_val_kb(m, "LowTotal:       ", i.totalram - i.totalhigh);
	show_val_kb(m, "LowFree:        ", i.freeram - i.freehigh);
#endif

#ifndef CONFIG_MMU
	show_val_kb(m, "MmapCopy:       ",
		    (unsigned long)atomic_long_read(&mmap_pages_allocated));
#endif

	show_val_kb(m, "SwapTotal:      ", i.totalswap);
	show_val_kb(m, "SwapFree:       ", i.freeswap);
	show_val_kb(m, "Dirty:          ",
		    global_node_page_state(NR_FILE_DIRTY));
	show_val_kb(m, "Writeback:      ",
		    global_node_page_state(NR_WRITEBACK));
	show_val_kb(m, "AnonPages:      ",
		    global_node_page_state(NR_ANON_MAPPED));
	show_val_kb(m, "Mapped:         ",
		    global_node_page_state(NR_FILE_MAPPED));
	show_val_kb(m, "Shmem:          ", i.sharedram);
	show_val_kb(m, "KReclaimable:   ", sreclaimable +
		    global_node_page_state(NR_KERNEL_MISC_RECLAIMABLE));
	show_val_kb(m, "Slab:           ", sreclaimable + sunreclaim);
	show_val_kb(m, "SReclaimable:   ", sreclaimable);
	show_val_kb(m, "SUnreclaim:     ", sunreclaim);
	seq_printf(m, "KernelStack:    %8lu kB\n",
		   global_zone_page_state(NR_KERNEL_STACK_KB));
	show_val_kb(m, "PageTables:     ",
		    global_zone_page_state(NR_PAGETABLE));
#ifdef CONFIG_QUICKLIST
	show_val_kb(m, "Quicklists:     ", quicklist_total_size());
#endif

	show_val_kb(m, "NFS_Unstable:   ",
		    global_node_page_state(NR_UNSTABLE_NFS));
	show_val_kb(m, "Bounce:         ",
		    global_zone_page_state(NR_BOUNCE));
	show_val_kb(m, "WritebackTmp:   ",
		    global_node_page_state(NR_WRITEBACK_TEMP));
	show_val_kb(m, "CommitLimit:    ", vm_commit_limit());
	show_val_kb(m, "Committed_AS:   ", committed);
	seq_printf(m, "VmallocTotal:   %8lu kB\n",
		   (unsigned long)VMALLOC_TOTAL >> 10);
	show_val_kb(m, "VmallocUsed:    ", 0ul);
	show_val_kb(m, "VmallocChunk:   ", 0ul);
	show_val_kb(m, "Percpu:         ", pcpu_nr_pages());

#ifdef CONFIG_MEMORY_FAILURE
	seq_printf(m, "HardwareCorrupted: %5lu kB\n",
		   atomic_long_read(&num_poisoned_pages) << (PAGE_SHIFT - 10));
#endif

#ifdef CONFIG_TRANSPARENT_HUGEPAGE
	show_val_kb(m, "AnonHugePages:  ",
		    global_node_page_state(NR_ANON_THPS) * HPAGE_PMD_NR);
	show_val_kb(m, "ShmemHugePages: ",
		    global_node_page_state(NR_SHMEM_THPS) * HPAGE_PMD_NR);
	show_val_kb(m, "ShmemPmdMapped: ",
		    global_node_page_state(NR_SHMEM_PMDMAPPED) * HPAGE_PMD_NR);
#endif

#ifdef CONFIG_CMA
	show_val_kb(m, "CmaTotal:       ", totalcma_pages);
	show_val_kb(m, "CmaFree:        ",
		    global_zone_page_state(NR_FREE_CMA_PAGES));
#endif
#endif  // CONFIG_POPCORN_REMOTE_INFO

	hugetlb_report_meminfo(m);

	arch_report_meminfo(m);

	return 0;
}

static int __init proc_meminfo_init(void)
{
	proc_create_single("meminfo", 0, NULL, meminfo_proc_show);
	return 0;
}
fs_initcall(proc_meminfo_init);
