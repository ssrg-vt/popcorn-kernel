#ifndef _LINUX_POPCORN_REMOTE_MEMINFO_H
#define _LINUX_POPCORN_REMOTE_MEMINFO_H

#include <popcorn/pcn_kmsg.h>

struct remote_mem_info_request {
	struct pcn_kmsg_hdr header;
	int nid;
	char pad_string[56];
} __packed __aligned(64);

struct remote_mem_info_response {
	struct pcn_kmsg_hdr header;
	int nid;

	unsigned long _MemTotal;
	unsigned long _MemFree;
	unsigned long _MemAvailable;
	unsigned long _Buffers;
	unsigned long _Cached;
	unsigned long _SwapCached;
	unsigned long _Active;
	unsigned long _Inactive;
	unsigned long _Active_anon;
	unsigned long _Inactive_anon;
	unsigned long _Active_file;
	unsigned long _Inactive_file;
	unsigned long _Unevictable;
	unsigned long _Mlocked;
	unsigned long _HighTotal;
	unsigned long _HighFree;
	unsigned long _LowTotal;
	unsigned long _LowFree;
	unsigned long _MmapCopy;
	unsigned long _SwapTotal;
	unsigned long _SwapFree;
	unsigned long _Dirty;
	unsigned long _Writeback;
	unsigned long _AnonPages;
	unsigned long _Mapped;
	unsigned long _Shmem;
	unsigned long _Slab;
	unsigned long _SReclaimable;
	unsigned long _SUnreclaim;
	unsigned long _KernelStack;
	unsigned long _PageTables;
	unsigned long _Quicklists;
	unsigned long _NFS_Unstable;
	unsigned long _Bounce;
	unsigned long _WritebackTmp;
	unsigned long _CommitLimit;
	unsigned long _Committed_AS;
	unsigned long _VmallocTotal;
	unsigned long _VmallocUsed;
	unsigned long _VmallocChunk;
	unsigned long _HardwareCorrupted;
	unsigned long _AnonHugePages;
	unsigned long _CmaTotal;
	unsigned long _CmaFree;
} __packed __aligned(64);

#endif
