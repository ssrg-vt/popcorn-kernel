/*
 * @file init.c
 *
 * Copyright (c) 2013 - 2014, Akshay
 * modified by Antonio Barbalace, 2014
 * rewritten by Sang-Hoon Kim, 2016-2017
 */

#include <linux/kernel.h>
#include <linux/workqueue.h>

#include <popcorn/debug.h>

struct workqueue_struct *popcorn_wq;
struct workqueue_struct *popcorn_ordered_wq;

extern int popcorn_ns_init(int);
extern int pcn_kmsg_init(void);
extern int popcorn_nodes_init(void);
extern int sched_server_init(void);
extern int process_server_init(void);
extern int vma_server_init(void);
extern int page_server_init(void);

static int __init popcorn_init(void)
{
	PRINTK(KERN_INFO"Initialize Popcorn subsystems...\n");

	/**
	 * Create work queues so that we can do bottom side
	 * processing on data that was brought in by the
	 * communications module interrupt handlers.
	 */
	popcorn_wq = create_workqueue("pcn_wq");
	popcorn_ordered_wq = create_singlethread_workqueue("pcn_wq_ordered");

	popcorn_ns_init(false);
	pcn_kmsg_init();

	popcorn_nodes_init();
	vma_server_init();
	process_server_init();
	page_server_init();
	sched_server_init();

	return 0;
}
late_initcall(popcorn_init);
