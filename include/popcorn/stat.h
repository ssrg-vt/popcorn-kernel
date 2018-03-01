/*
 * stat.h
 *
 *  Created on: May 22, 2017
 *      Author: Sang-Hoon Kim (sanghoon@vt.edu)
 */

#ifndef __KERNEL_POPCORN_STAT_H__
#define __KERNEL_POPCORN_STAT_H__

struct pcn_kmsg_message;

void account_pcn_message_sent(struct pcn_kmsg_message *msg);
void account_pcn_message_recv(struct pcn_kmsg_message *msg);

void account_pcn_rdma_write(size_t size);
void account_pcn_rdma_read(size_t size);

#endif /* KERNEL_POPCORN_STAT_H_ */
