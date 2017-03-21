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

#define REMOTE_MEMINFO_VERBOSE 1
#if REMOTE_MEMINFO_VERBOSE
#define MEMPRINTK(...) printk(__VA_ARGS__)
#else
#define MEMPRINTK(...)
#endif

static DECLARE_WAIT_QUEUE_HEAD(wq_mem);
static int wait_mem_list = -1;

static struct remote_mem_info_response *saved_mem_info[MAX_POPCORN_NODES];

int fill_meminfo_response(struct remote_mem_info_response *res)
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


	res->_MemTotal = K(i.totalram);
	res->_MemFree = K(i.freeram);
	res->_MemAvailable = K(available);
	res->_Buffers = K(i.bufferram);
	res->_Cached = K(cached);
	res->_SwapCached = K(total_swapcache_pages());
	res->_Active = K(pages[LRU_ACTIVE_ANON]   + pages[LRU_ACTIVE_FILE]);
	res->_Inactive = K(pages[LRU_INACTIVE_ANON] + pages[LRU_INACTIVE_FILE]);
	res->_Active_anon = K(pages[LRU_ACTIVE_ANON]);
	res->_Inactive_anon = K(pages[LRU_INACTIVE_ANON]);
	res->_Active_file = K(pages[LRU_ACTIVE_FILE]);
	res->_Inactive_file = K(pages[LRU_INACTIVE_FILE]);
	res->_Unevictable = K(pages[LRU_UNEVICTABLE]);
	res->_Mlocked = K(global_page_state(NR_MLOCK));
#ifdef CONFIG_HIGHMEM
	res->_HighTotal = K(i.totalhigh);
	res->_HighFree = K(i.freehigh);
	res->_LowTotal = K(i.totalram-i.totalhigh);
	res->_LowFree = K(i.freeram-i.freehigh);
#endif
#ifndef CONFIG_MMU
	res->rem_mem._MmapCopy = K((unsigned long) atomic_long_read(&mmap_pages_allocated));
#endif
	res->_SwapTotal = K(i.totalswap);
	res->_SwapFree = K(i.freeswap);
	res->_Dirty = K(global_page_state(NR_FILE_DIRTY));
	res->_Writeback = K(global_page_state(NR_WRITEBACK));
	res->_AnonPages = K(global_page_state(NR_ANON_PAGES));
	res->_Mapped = K(global_page_state(NR_FILE_MAPPED));
	res->_Shmem = K(i.sharedram);
	res->_Slab = K(global_page_state(NR_SLAB_RECLAIMABLE) +
				global_page_state(NR_SLAB_UNRECLAIMABLE));
	res->_SReclaimable = K(global_page_state(NR_SLAB_RECLAIMABLE));
	res->_SUnreclaim = K(global_page_state(NR_SLAB_UNRECLAIMABLE));
	res->_KernelStack = global_page_state(NR_KERNEL_STACK) * THREAD_SIZE / 1024;
	res->_PageTables = K(global_page_state(NR_PAGETABLE));
#ifdef CONFIG_QUICKLIST
	res->_Quicklists = K(quicklist_total_size());
#endif
	res->_NFS_Unstable = K(global_page_state(NR_UNSTABLE_NFS));
	res->_Bounce = K(global_page_state(NR_BOUNCE));
	res->_WritebackTmp = K(global_page_state(NR_WRITEBACK_TEMP));
	res->_CommitLimit = K(vm_commit_limit());
	res->_Committed_AS = K(committed);
	res->_VmallocTotal = (unsigned long)VMALLOC_TOTAL >> 10;
	res->_VmallocUsed = 0ul;
	res->_VmallocChunk = 0ul;
#ifdef CONFIG_MEMORY_FAILURE
	res->_HardwareCorrupted = atomic_long_read(&num_poisoned_pages) << (PAGE_SHIFT - 10);
#endif
#ifdef CONFIG_TRANSPARENT_HUGEPAGE
	res->_AnonHugePages = K(global_page_state(NR_ANON_TRANSPARENT_HUGEPAGES) *
		   HPAGE_PMD_NR);
#endif
#ifdef CONFIG_CMA
	res->_CmaTotal = K(totalcma_pages);
	res->_CmaFree = K(global_page_state(NR_FREE_CMA_PAGES));
#endif

	return 0;
}

static int handle_remote_mem_info_request(struct pcn_kmsg_message *inc_msg)
{
	struct remote_mem_info_request *request;
	struct remote_mem_info_response *response;
	int ret;

	MEMPRINTK("%s: Entered\n", __func__);

	request = (struct remote_mem_info_request *)inc_msg;
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
	struct remote_mem_info_response *response;

	MEMPRINTK("%s: Entered\n", __func__);

	wait_mem_list = 1;

	response = (struct remote_mem_info_response *)inc_msg;
	if (response == NULL) {
		MEMPRINTK("%s: NULL pointer\n", __func__);
		return -EINVAL;
	}

	/* 1. Save remote mem info from remote node */
	memcpy(saved_mem_info[response->nid], response, sizeof(*response));

	/* 2. Wake up request message waiting */
	wake_up_interruptible(&wq_mem);

	/* 3. Remove response message received from remote node */
	pcn_kmsg_free_msg(response);

	MEMPRINTK("%s: done\n", __func__);
	return 0;
}

int remote_mem_info_init(void)
{
	int i = 0;

	/* Allocate the buffer for saving remote memory info */
	for (i = 0; i < MAX_POPCORN_NODES; i++)
		saved_mem_info[i] = kzalloc(sizeof(struct remote_mem_info_response),
					    GFP_KERNEL);

	/* Register callbacks for both request and response */
	pcn_kmsg_register_callback(PCN_KMSG_TYPE_REMOTE_PROC_MEMINFO_REQUEST,
				   handle_remote_mem_info_request);
	pcn_kmsg_register_callback(PCN_KMSG_TYPE_REMOTE_PROC_MEMINFO_RESPONSE,
				   handle_remote_mem_info_response);

	MEMPRINTK("%s: done\n", __func__);
	return 0;
}

int send_remote_mem_info_request(unsigned int nid)
{
	struct remote_mem_info_request *request;
	int ret = 0;

	MEMPRINTK("%s: Entered, nid: %d\n", __func__, nid);

	wait_mem_list = -1;

	request = kzalloc(sizeof(*request), GFP_KERNEL);

	/* 1. Construct request data to send it into remote node */

	/* 1-1. Fill the header information */
	request->header.type = PCN_KMSG_TYPE_REMOTE_PROC_MEMINFO_REQUEST;
	request->header.prio = PCN_KMSG_PRIO_NORMAL;
	request->nid = my_nid;

	/* 1-2. Send request into remote node */
	ret = pcn_kmsg_send_long(nid, request, sizeof(*request));
	if (ret < 0) {
		MEMPRINTK("%s: failed to send request message\n", __func__);
		goto out;
	}

	MEMPRINTK("%s: done\n", __func__);
out:
	kfree(request);
	return 0;
}

int remote_proc_mem_info(struct remote_mem_info_response *total)
{
	int i;
	struct remote_mem_info_response *meminfo_result;

	if (total == NULL)
		return -EINVAL;

	memset(total, 0, sizeof(struct remote_mem_info_response));

	for (i = 0; i < MAX_POPCORN_NODES; i++) {
		if (i == my_nid)
			i++;

		if (!is_popcorn_node_online(i))
			continue;

		send_remote_mem_info_request(i);

		wait_event_interruptible(wq_mem, wait_mem_list != -1);
		wait_mem_list = -1;

		meminfo_result = saved_mem_info[i];
		if (meminfo_result == NULL)
			return -EINVAL;

		total->_MemTotal += meminfo_result->_MemTotal;
		total->_MemFree += meminfo_result->_MemFree;
		total->_MemAvailable += meminfo_result->_MemAvailable;
		total->_Buffers += meminfo_result->_Buffers;
		total->_Cached += meminfo_result->_Cached;
		total->_SwapCached += meminfo_result->_SwapCached;
		total->_Active += meminfo_result->_Active;
		total->_Inactive += meminfo_result->_Inactive;
		total->_Active_anon += meminfo_result->_Active_anon;
		total->_Inactive_anon += meminfo_result->_Inactive_anon;
		total->_Active_file += meminfo_result->_Active_file;
		total->_Inactive_file += meminfo_result->_Inactive_file;
		total->_Unevictable += meminfo_result->_Unevictable;
		total->_Mlocked += meminfo_result->_Mlocked;

#ifdef CONFIG_HIGHMEM
		total->_HighTotal += meminfo_result->_HighTotal;
		total->_HighFre += meminfo_result->_HighFree;
		total->_LowTotal += meminfo_result->_LowTotal;
		total->_LowFree += meminfo_result->_LowFree;
#endif

#ifndef CONFIG_MMU
		total->_MmapCopy += meminfo_result->_MmapCopy;
#endif

		total->_SwapTotal += meminfo_result->_SwapTotal;
		total->_SwapFree += meminfo_result->_SwapFree;
		total->_Dirty += meminfo_result->_Dirty;
		total->_Writeback += meminfo_result->_Writeback;
		total->_AnonPages += meminfo_result->_AnonPages;
		total->_Mapped += meminfo_result->_Mapped;
		total->_Shmem += meminfo_result->_Shmem;
		total->_Slab += meminfo_result->_Slab;
		total->_SReclaimable += meminfo_result->_SReclaimable;
		total->_SUnreclaim += meminfo_result->_SUnreclaim;
		total->_KernelStack += meminfo_result->_KernelStack;
		total->_PageTables += meminfo_result->_PageTables;
#ifdef CONFIG_QUICKLIST
		total->_Quicklists += meminfo_result->_Quicklists;
#endif

		total->_NFS_Unstable += meminfo_result->_NFS_Unstable;
		total->_Bounce += meminfo_result->_Bounce;
		total->_WritebackTmp += meminfo_result->_WritebackTmp;
		total->_CommitLimit += meminfo_result->_CommitLimit;
		total->_Committed_AS += meminfo_result->_Committed_AS;
		total->_VmallocTotal += meminfo_result->_VmallocTotal;
		total->_VmallocUsed += 0ul;
		total->_VmallocChunk += 0ul;
#ifdef CONFIG_MEMORY_FAILURE
		total->_HardwareCorrupted += meminfo_result->_HardwareCorrupted;
#endif
#ifdef CONFIG_TRANSPARENT_HUGEPAGE
		total->_AnonHugePages += meminfo_result->_AnonHugePages;
#endif
#ifdef CONFIG_CMA
		total->_CmaTotal += meminfo_result->_CmaTotal;
		total->_CmaFree += meminfo_result->_CmaFree;
#endif
	}

	return 0;
}
