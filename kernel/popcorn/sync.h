/*
 * sync.h
 * Copyright (C) 2018 jackchuang <jackchuang@mir7>
 *
 * Distributed under terms of the MIT license.
 */

#ifndef _SYNC_H
#define _SYNC_H
#include <popcorn/sync.h>
void tso_wr_inc(struct vm_area_struct *vma, unsigned long addr, struct page *page, spinlock_t *ptl);

#endif /* !_SYNC_H */
