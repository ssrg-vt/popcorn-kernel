/*
 * sync.h
 * Copyright (C) 2018 Ho-Ren (Jack) Chuang <horenc@vt.edu>
 *
 * Distributed under terms of the MIT license.
 */

#ifndef SYNC_H
#define SYNC_H
#include <popcorn/pcn_kmsg.h>

#define MAX_OMP_REGIONS 100

/* Depends on (PCN_KMSG_MAX_SIZE - 1) pages for msg head + metadata */
#define LIMIT_PER_INV_ADDR_SIZE_FACTOR (PCN_KMSG_MAX_SIZE / (32UL << 10)) // for seting inv cnt as the same as when size = 32k
#define MAX_WRITE_INV_BUFFERS ((long unsigned int)((PCN_KMSG_MAX_PAYLOAD_SIZE / LIMIT_PER_INV_ADDR_SIZE_FACTOR) / sizeof(unsigned long)) - 8) // (-8) since page_merge_request_t has 8 element each has 8 bytes (worst case)

/* IS-D, BT-D use more: 2000 => 2000 * 96(max threads)
 * For not IS-D: 1500
 * 2500 will crash since cannot allocate sys_region
 */
#define MAX_READ_BUFFERS 2000
#define MAX_WRITE_NOPAGE_BUFFERS 2000

#if VM_TESTING
#define X86_THREADS 8
#define ARM_THREADS 8
#else
#define X86_THREADS 16
#define ARM_THREADS 96
//#define X86_THREADS 24
//#define ARM_THREADS 144
#endif

/* 1 end spot for sorting */
#define MAX_ALIVE_THREADS (X86_THREADS + ARM_THREADS + 1)
/*
#if VM_TESTING
#define MAX_ALIVE_THREADS (16 + 1)
#else
//#define MAX_ALIVE_THREADS (112 + 1)
//#define MAX_ALIVE_THREADS (168 + 1)
#define MAX_ALIVE_THREADS (X86_THREADS + ARM_THREADS + 1)
// try X86_THREADS + ARM_THREADS
#endif
*/
#endif /* !SYNC_H */
