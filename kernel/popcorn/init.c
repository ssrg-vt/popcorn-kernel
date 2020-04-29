// SPDX-License-Identifier: GPL-2.0, BSD
/*
 * /kernel/popcorn/init.c
 *
 * Popcorn node init
 *
 * Copyright (c) 2013 - 2014 Akshay Giridhar
 *
 * author, Javier Malave, Rebecca Shapiro, Andrew Hughes,
 * Narf Industries 2020 (modifications for upstream RFC)
 * author, rewritten by Sang-Hoon Kim, 2016-2017
 * author, modified by Antonio Barbalace, 2014
 */

#include <linux/kernel.h>
#include <linux/workqueue.h>

#include <popcorn/debug.h>
#include "types.h"

#define CREATE_TRACE_POINTS
#include "trace_events.h"

struct workqueue_struct *popcorn_wq;
struct workqueue_struct *popcorn_ordered_wq;
EXPORT_SYMBOL(popcorn_wq);
EXPORT_SYMBOL(popcorn_ordered_wq);

extern int pcn_kmsg_init(void);
extern int popcorn_nodes_init(void);
extern int process_server_init(void);
extern int vma_server_init(void);
extern int page_server_init(void);
extern int statistics_init(void);

static int __init popcorn_init(void)
{
	PRINTK("Initialize Popcorn subsystems...\n");

	/*
	 * Create work queues so that we can do bottom side
	 * processing on data that was brought in by the
	 * communications module interrupt handlers.
	 */
	popcorn_ordered_wq = create_singlethread_workqueue("pcn_wq_ordered");
	popcorn_wq = alloc_workqueue("pcn_wq", WQ_MEM_RECLAIM, 0);

	pcn_kmsg_init();

	popcorn_nodes_init();
	vma_server_init();
	process_server_init();
	page_server_init();

	statistics_init();
	return 0;
}
late_initcall(popcorn_init);
