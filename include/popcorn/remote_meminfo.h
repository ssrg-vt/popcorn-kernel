#ifndef _LINUX_POPCORN_REMOTE_MEMINFO_H
#define _LINUX_POPCORN_REMOTE_MEMINFO_H

#include <popcorn/pcn_kmsg.h>

#define REMOTE_MEMINFO_REQUEST_FIELDS \
	int nid; \
	int origin_ws;
DEFINE_PCN_KMSG(remote_mem_info_request_t, REMOTE_MEMINFO_REQUEST_FIELDS);

#define REMOTE_MEMINFO_RESPONSE_FIELDS \
	int nid; \
	int origin_ws; \
	unsigned long MemTotal; \
	unsigned long MemFree; \
	unsigned long MemAvailable; \
	unsigned long Buffers; \
	unsigned long Cached; \
	unsigned long SwapCached; \
	unsigned long Active; \
	unsigned long Inactive; \
	unsigned long Active_anon; \
	unsigned long Inactive_anon; \
	unsigned long Active_file; \
	unsigned long Inactive_file; \
	unsigned long Unevictable; \
	unsigned long Mlocked; \
	unsigned long HighTotal; \
	unsigned long HighFree; \
	unsigned long LowTotal; \
	unsigned long LowFree; \
	unsigned long MmapCopy; \
	unsigned long SwapTotal; \
	unsigned long SwapFree; \
	unsigned long Dirty; \
	unsigned long Writeback; \
	unsigned long AnonPages; \
	unsigned long Mapped; \
	unsigned long Shmem; \
	unsigned long Slab; \
	unsigned long SReclaimable; \
	unsigned long SUnreclaim; \
	unsigned long KernelStack; \
	unsigned long PageTables; \
	unsigned long Quicklists; \
	unsigned long NFS_Unstable; \
	unsigned long Bounce; \
	unsigned long WritebackTmp; \
	unsigned long CommitLimit; \
	unsigned long Committed_AS; \
	unsigned long VmallocTotal; \
	unsigned long VmallocUsed; \
	unsigned long VmallocChunk; \
	unsigned long HardwareCorrupted; \
	unsigned long AnonHugePages; \
	unsigned long CmaTotal; \
	unsigned long CmaFree;
DEFINE_PCN_KMSG(remote_mem_info_response_t, REMOTE_MEMINFO_RESPONSE_FIELDS);

int remote_proc_mem_info(remote_mem_info_response_t *total);

#endif
