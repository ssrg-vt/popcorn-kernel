// SPDX-License-Identifier: GPL-2.0, 3-clause BSD
#ifndef __KERNEL_POPCORN_STAT_H__
#define __KERNEL_POPCORN_STAT_H__

struct pcn_kmsg_message;

void account_pcn_message_sent(struct pcn_kmsg_message *msg);
void account_pcn_message_recv(struct pcn_kmsg_message *msg);

void account_pcn_rdma_write(size_t size);
void account_pcn_rdma_read(size_t size);

#define POPCORN_STAT_FMT  "%12llu  %12llu  %s\n"
#define POPCORN_STAT_FMT2 "%8llu.%03llu  %8llu.%03llu  %s\n"

#endif /* KERNEL_POPCORN_STAT_H_ */
