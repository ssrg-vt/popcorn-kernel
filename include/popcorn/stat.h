/*
 * stat.h
 *
 *  Created on: May 22, 2017
 *      Author: Sang-Hoon Kim (sanghoon@vt.edu)
 */

#ifndef __KERNEL_POPCORN_STAT_H__
#define __KERNEL_POPCORN_STAT_H__

struct pcn_kmsg_message;

enum stat_item {
	STAT_PAGE_FETCH,
	STAT_PAGE_INVALIDATE,
	STAT_VMA_FETCH,

	STAT_PAGE_GRANT,
	STAT_ENTRY_MAX,
};

void inc_popcorn_stat(enum stat_item i);
void add_popcorn_stat(enum stat_item i, int n);
void account_pcn_message_sent(struct pcn_kmsg_message *msg);
void account_pcn_message_recv(struct pcn_kmsg_message *msg);

void print_popcorn_stat(void);

#endif /* KERNEL_POPCORN_STAT_H_ */
