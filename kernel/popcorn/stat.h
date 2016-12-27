/*
 * stat.h
 *
 *  Created on: May 22, 2016
 *      Author: root
 */

#ifndef KERNEL_POPCORN_STAT_H_
#define KERNEL_POPCORN_STAT_H_

#if MIGRATION_PROFILE
ktime_t migration_start;
ktime_t migration_end;
#endif

//asmlinkage 
long sys_take_time(int start)
{
	if (start == 1)
		trace_printk("s\n");
	else
		trace_printk("e\n");
	return 0;
}

#if STATISTICS
//unsigned long long perf_aa, perf_bb, perf_cc, perf_dd, perf_ee;
static int page_fault_mio, fetch, local_fetch, write, concurrent_write, most_long_write;
static int most_written_page, read, most_long_read, invalid, ack, answer_request, answer_request_void;
static int request_data, pages_allocated, compressed_page_sent, not_compressed_page, not_compressed_diff_page;
#endif

#endif /* KERNEL_POPCORN_STAT_H_ */
