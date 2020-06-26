/*
 * sync.h
 * Copyright (C) 2018 Ho-Ren (Jack) Chuang <horenc@vt.edu>
 *
 * Distributed under terms of the MIT license.
 */

#ifndef _SYNC_H
#define _SYNC_H
#include <popcorn/sync.h>
void tso_wr_inc(struct vm_area_struct *vma, unsigned long addr, struct page *page, spinlock_t *ptl);

#if 0 // moved to inc;lude/popconr/ sync.h testing now
#if VM_TESTING
#define X86_THREADS 8
#define ARM_THREADS 8
#else
//#define X86_THREADS 16
//#define ARM_THREADS 96
#define X86_THREADS 24
#define ARM_THREADS 144
#endif
#endif

#define MAX_POPCORN_THREADS ARM_THREADS
#define MAX_PF_MSG (ARM_THREADS * 10 * 2) // = (1000msg * 31pg per msg) pages is enough except sp (2000)
#endif /* !_SYNC_H */
