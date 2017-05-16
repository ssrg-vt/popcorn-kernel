/**
 * @file remote_mem_info.c
 *
 * Popcorn Linux remote meminfo implementation
 * This work is a rework of Akshay Giridhar's implementation
 * to provide the proc/meminfo for remote nodes.
 *
 * @author Jingoo Han, SSRG Virginia Tech 2017
 */

#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/hugetlb.h>
#include <linux/mman.h>
#include <linux/mmzone.h>
#include <linux/proc_fs.h>
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

#include <linux/printk.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/wait.h>

#include <popcorn/bundle.h>
#include <popcorn/pcn_kmsg.h>
#include <popcorn/remote_meminfo.h>

#include "wait_station.h"

#define REMOTE_MEMINFO_VERBOSE 0
#if REMOTE_MEMINFO_VERBOSE
#define MEMPRINTK(...) printk(__VA_ARGS__)
#else
#define MEMPRINTK(...)
#endif

int fill_meminfo_response(remote_mem_info_response_t *res)
{
	struct sysinfo i;
	unsigned long committed;
	long cached;
	long available;
	unsigned long pagecache;
	unsigned long wmark_low = 0;
	unsigned long pages[NR_LRU_LISTS];
	struct zone *zone;
	int lru;

/*
 * display in kilobytes.
 */
#define K(x) ((x) << (PAGE_SHIFT - 10))
	si_meminfo(&i);
	si_swapinfo(&i);
	committed = percpu_counter_read_positive(&vm_committed_as);

	cached = global_page_state(NR_FILE_PAGES) -
			total_swapcache_pages() - i.bufferram;
	if (cached < 0)
		cached = 0;

	for (lru = LRU_BASE; lru < NR_LRU_LISTS; lru++)
		pages[lru] = global_page_state(NR_LRU_BASE + lru);

	for_each_zone(zone)
		wmark_low += zone->watermark[WMARK_LOW];

	/*
	 * Estimate the amount of memory available for userspace allocations,
	 * without causing swapping.
	 *
	 * Free memory cannot be taken below the low watermark, before the
	 * system starts swapping.
	 */
	available = i.freeram - wmark_low;

	/*
	 * Not all the page cache can be freed, otherwise the system will
	 * start swapping. Assume at least half of the page cache, or the
	 * low watermark worth of cache, needs to stay.
	 */
	pagecache = pages[LRU_ACTIVE_FILE] + pages[LRU_INACTIVE_FILE];
	pagecache -= min(pagecache / 2, wmark_low);
	available += pagecache;

	/*
	 * Part of the reclaimable slab consists of items that are in use,
	 * and cannot be freed. Cap this estimate at the low watermark.
	 */
	available += global_page_state(NR_SLAB_RECLAIMABLE) -
		     min(global_page_state(NR_SLAB_RECLAIMABLE) / 2, wmark_low);

	if (available < 0)
		available = 0;

	/* Fill the mem information for response */
	res->MemTotal = K(i.totalram);
	res->MemFree = K(i.freeram);
	res->MemAvailable = K(available);
	res->Buffers = K(i.bufferram);
	res->Cached = K(cached);
	res->SwapCached = K(total_swapcache_pages());
	res->Active = K(pages[LRU_ACTIVE_ANON]   + pages[LRU_ACTIVE_FILE]);
	res->Inactive = K(pages[LRU_INACTIVE_ANON] + pages[LRU_INACTIVE_FILE]);
	res->Active_anon = K(pages[LRU_ACTIVE_ANON]);
	res->Inactive_anon = K(pages[LRU_INACTIVE_ANON]);
	res->Active_file = K(pages[LRU_ACTIVE_FILE]);
	res->Inactive_file = K(pages[LRU_INACTIVE_FILE]);
	res->Unevictable = K(pages[LRU_UNEVICTABLE]);
	res->Mlocked = K(global_page_state(NR_MLOCK));
#ifdef CONFIG_HIGHMEM
	res->HighTotal = K(i.totalhigh);
	res->HighFree = K(i.freehigh);
	res->LowTotal = K(i.totalram-i.totalhigh);
	res->LowFree = K(i.freeram-i.freehigh);
#endif
#ifndef CONFIG_MMU
	res->rem_mem.MmapCopy = K((unsigned long) atomic_long_read(&mmap_pages_allocated));
#endif
	res->SwapTotal = K(i.totalswap);
	res->SwapFree = K(i.freeswap);
	res->Dirty = K(global_page_state(NR_FILE_DIRTY));
	res->Writeback = K(global_page_state(NR_WRITEBACK));
	res->AnonPages = K(global_page_state(NR_ANON_PAGES));
	res->Mapped = K(global_page_state(NR_FILE_MAPPED));
	res->Shmem = K(i.sharedram);
	res->Slab = K(global_page_state(NR_SLAB_RECLAIMABLE) +
				global_page_state(NR_SLAB_UNRECLAIMABLE));
	res->SReclaimable = K(global_page_state(NR_SLAB_RECLAIMABLE));
	res->SUnreclaim = K(global_page_state(NR_SLAB_UNRECLAIMABLE));
	res->KernelStack = global_page_state(NR_KERNEL_STACK) * THREAD_SIZE / 1024;
	res->PageTables = K(global_page_state(NR_PAGETABLE));
#ifdef CONFIG_QUICKLIST
	res->Quicklists = K(quicklist_total_size());
#endif
	res->NFS_Unstable = K(global_page_state(NR_UNSTABLE_NFS));
	res->Bounce = K(global_page_state(NR_BOUNCE));
	res->WritebackTmp = K(global_page_state(NR_WRITEBACK_TEMP));
	res->CommitLimit = K(vm_commit_limit());
	res->Committed_AS = K(committed);
	res->VmallocTotal = (unsigned long)VMALLOC_TOTAL >> 10;
	res->VmallocUsed = 0ul;
	res->VmallocChunk = 0ul;
#ifdef CONFIG_MEMORY_FAILURE
	res->HardwareCorrupted = atomic_long_read(&num_poisoned_pages) << (PAGE_SHIFT - 10);
#endif
#ifdef CONFIG_TRANSPARENT_HUGEPAGE
	res->AnonHugePages = K(global_page_state(NR_ANON_TRANSPARENT_HUGEPAGES) *
		   HPAGE_PMD_NR);
#endif
#ifdef CONFIG_CMA
	res->CmaTotal = K(totalcma_pages);
	res->CmaFree = K(global_page_state(NR_FREE_CMA_PAGES));
#endif

	return 0;
}

static int handle_remote_mem_info_request(struct pcn_kmsg_message *inc_msg)
{
	remote_mem_info_request_t *request;
	remote_mem_info_response_t *response;
	int ret;

	MEMPRINTK("%s: Entered\n", __func__);

	request = (remote_mem_info_request_t *)inc_msg;
	if (request == NULL) {
		MEMPRINTK("%s: NULL pointer\n", __func__);
		return -EINVAL;
	}

	response = kzalloc(sizeof(*response), GFP_KERNEL);

	/* 1. Construct response data to send it into remote node */

	/* 1-1. Fill the header information */
	response->header.type = PCN_KMSG_TYPE_REMOTE_PROC_MEMINFO_RESPONSE;
	response->header.prio = PCN_KMSG_PRIO_NORMAL;
	response->nid = my_nid;

	/* 1-2. Fill the machine-dependent MEMORY information */
	ret = fill_meminfo_response(response);
	if (ret < 0) {
		MEMPRINTK("%s: failed to fill memory info\n", __func__);
		goto out;
	}

	/* 1-3. Send response into remote node */
	ret = pcn_kmsg_send_long(request->nid, response, sizeof(*response));
	if (ret < 0) {
		MEMPRINTK("%s: failed to send response message\n", __func__);
		goto out;
	}

	/* 2. Remove request message received from remote node */
	pcn_kmsg_free_msg(request);

	MEMPRINTK("%s: done\n", __func__);
out:
	kfree(response);
	return 0;
}

static int handle_remote_mem_info_response(struct pcn_kmsg_message *inc_msg)
{
	remote_mem_info_response_t *response;
	struct wait_station *ws;

	MEMPRINTK("%s: Entered\n", __func__);

	response = (remote_mem_info_response_t *)inc_msg;
	if (response == NULL) {
		MEMPRINTK("%s: NULL pointer\n", __func__);
		return -EINVAL;
	}

	ws = wait_station(response->origin_ws);
	ws->private = response;

	smp_mb();

	if (atomic_dec_and_test(&ws->pendings_count))
		complete(&ws->pendings);

	MEMPRINTK("%s: done\n", __func__);
	return 0;
}

int remote_mem_info_init(void)
{
	/* Register callbacks for both request and response */
	pcn_kmsg_register_callback(PCN_KMSG_TYPE_REMOTE_PROC_MEMINFO_REQUEST,
				   handle_remote_mem_info_request);
	pcn_kmsg_register_callback(PCN_KMSG_TYPE_REMOTE_PROC_MEMINFO_RESPONSE,
				   handle_remote_mem_info_response);

	MEMPRINTK("%s: done\n", __func__);
	return 0;
}

remote_mem_info_response_t *send_remote_mem_info_request(struct task_struct *tsk,
							 unsigned int nid)
{
	remote_mem_info_request_t *request;
	remote_mem_info_response_t *response;
	struct wait_station *ws = get_wait_station(tsk->pid, 1);

	MEMPRINTK("%s: Entered, nid: %d\n", __func__, nid);

	request = kzalloc(sizeof(*request), GFP_KERNEL);

	/* 1. Construct request data to send it into remote node */

	/* 1-1. Fill the header information */
	request->header.type = PCN_KMSG_TYPE_REMOTE_PROC_MEMINFO_REQUEST;
	request->header.prio = PCN_KMSG_PRIO_NORMAL;
	request->nid = my_nid;
	request->origin_ws = ws->id;

	/* 1-2. Send request into remote node */
	pcn_kmsg_send_long(nid, request, sizeof(*request));

	wait_at_station(ws);
	response = ws->private;
	put_wait_station(ws);

	kfree(request);

	MEMPRINTK("%s: done\n", __func__);
	return response;
}

int remote_proc_mem_info(remote_mem_info_response_t *total)
{
	int i;
	remote_mem_info_response_t *meminfo_result;

	if (total == NULL)
		return -EINVAL;

	memset(total, 0, sizeof(remote_mem_info_response_t));

	for (i = 0; i < MAX_POPCORN_NODES; i++) {
		if (i == my_nid)
			i++;

		if (!is_popcorn_node_online(i))
			continue;

		meminfo_result = send_remote_mem_info_request(current, i);
		if (meminfo_result == NULL)
			return -EINVAL;

		total->MemTotal += meminfo_result->MemTotal;
		total->MemFree += meminfo_result->MemFree;
		total->MemAvailable += meminfo_result->MemAvailable;
		total->Buffers += meminfo_result->Buffers;
		total->Cached += meminfo_result->Cached;
		total->SwapCached += meminfo_result->SwapCached;
		total->Active += meminfo_result->Active;
		total->Inactive += meminfo_result->Inactive;
		total->Active_anon += meminfo_result->Active_anon;
		total->Inactive_anon += meminfo_result->Inactive_anon;
		total->Active_file += meminfo_result->Active_file;
		total->Inactive_file += meminfo_result->Inactive_file;
		total->Unevictable += meminfo_result->Unevictable;
		total->Mlocked += meminfo_result->Mlocked;

#ifdef CONFIG_HIGHMEM
		total->HighTotal += meminfo_result->HighTotal;
		total->HighFre += meminfo_result->HighFree;
		total->LowTotal += meminfo_result->LowTotal;
		total->LowFree += meminfo_result->LowFree;
#endif

#ifndef CONFIG_MMU
		total->MmapCopy += meminfo_result->MmapCopy;
#endif

		total->SwapTotal += meminfo_result->SwapTotal;
		total->SwapFree += meminfo_result->SwapFree;
		total->Dirty += meminfo_result->Dirty;
		total->Writeback += meminfo_result->Writeback;
		total->AnonPages += meminfo_result->AnonPages;
		total->Mapped += meminfo_result->Mapped;
		total->Shmem += meminfo_result->Shmem;
		total->Slab += meminfo_result->Slab;
		total->SReclaimable += meminfo_result->SReclaimable;
		total->SUnreclaim += meminfo_result->SUnreclaim;
		total->KernelStack += meminfo_result->KernelStack;
		total->PageTables += meminfo_result->PageTables;
#ifdef CONFIG_QUICKLIST
		total->Quicklists += meminfo_result->Quicklists;
#endif

		total->NFS_Unstable += meminfo_result->NFS_Unstable;
		total->Bounce += meminfo_result->Bounce;
		total->WritebackTmp += meminfo_result->WritebackTmp;
		total->CommitLimit += meminfo_result->CommitLimit;
		total->Committed_AS += meminfo_result->Committed_AS;
		total->VmallocTotal += meminfo_result->VmallocTotal;
		total->VmallocUsed += 0ul;
		total->VmallocChunk += 0ul;
#ifdef CONFIG_MEMORY_FAILURE
		total->HardwareCorrupted += meminfo_result->HardwareCorrupted;
#endif
#ifdef CONFIG_TRANSPARENT_HUGEPAGE
		total->AnonHugePages += meminfo_result->AnonHugePages;
#endif
#ifdef CONFIG_CMA
		total->CmaTotal += meminfo_result->CmaTotal;
		total->CmaFree += meminfo_result->CmaFree;
#endif

		pcn_kmsg_free_msg(meminfo_result);
	}

	return 0;
}
