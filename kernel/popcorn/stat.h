/*
 * stat.h
 *
 *  Created on: May 22, 2016
 *      Author: Sang-Hoon Kim (sanghoon@vt.edu)
 */

#ifndef __KERNEL_POPCORN_STAT_H__
#define __KERNEL_POPCORN_STAT_H__

#define STATISTICS 0

#ifdef MIGRATION_PROFILE
extern ktime_t migration_start;
extern ktime_t migration_end;
#define GET_MIGRATION_TIME \
	(unsigned long)ktime_to_ns(ktime_sub(migration_end, migration_start))

#endif

#if defined(CONFIG_ARM64)
#define MY_ARCH	"ARM"
#elif defined(CONFIG_X86) || defined(CONFIG_X86_64)
#define MY_ARCH	"x86"
#endif

extern int page_fault_mio;
extern int fetch;
extern int local_fetch;
extern int write;
extern int concurrent_write;
extern int most_written_page;
extern int read;
extern int invalid;
extern int ack;
extern int answer_request;
extern int answer_request_void;
extern int request_data;
extern int pages_allocated;
extern int most_long_write;
extern int most_long_read;
extern int compressed_page_sent;
extern int not_compressed_page;
extern int not_compressed_diff_page;

void print_popcorn_stat(void);

#endif /* KERNEL_POPCORN_STAT_H_ */
