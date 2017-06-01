/*
 * common.h
 * Copyright (C) 2017 jackchuang <jackchuang@echo3>
 *
 * Distributed under terms of the MIT license.
 */

#ifndef _MSG_LAYER_COMMON_H_
#define _MSG_LAYER_COMMON_H_

#include <popcorn/pcn_kmsg.h>
#include <popcorn/bundle.h>
#include <popcorn/debug.h>

/* ip */
#include <linux/net.h>
#include <linux/inet.h>
#include <linux/inetdevice.h>

#define MAX_NUM_NODES		2	// For development convenience
#define MAX_NUM_CHANNELS	(MAX_NUM_NODES - 1)

//const char * const ip_addresses[] = {
char * const ip_addresses[] = {
	/* ib */
	"192.168.69.129",	// echo5 ib0
	"192.168.69.128",	// echo6 ib0
	/* dolphin */
    //"10.4.4.15",
    //"10.4.4.16",
	/* socket */
    //"10.1.1.203",
    //"10.1.1.204",
    //"10.1.1.205",
};

/* only match the first one */
char *net_dev_names[] = {
    "ib0",      // InfiniBand
    //"eth0",     // Socket/dolphin
    //"br0",      // bridge
    //"p7p1",     // Xgene (ARM)
};

uint32_t ip_table[MAX_NUM_NODES] = { 0 };

/* utilityes */
uint32_t get_host_ip(char **name_ret)
{
    int i;
    for (i = 0; i < sizeof(net_dev_names) / sizeof(*net_dev_names); i++) {
		struct net_device *device;
        struct in_device *in_dev;
        char *name = net_dev_names[i];
        device = dev_get_by_name(&init_net, name); // namespace=normale

        if (device) {
            struct in_ifaddr *if_info;

            *name_ret = name;
            in_dev = (struct in_device *)device->ip_ptr;
            if_info = in_dev->ifa_list;

            MSGDPRINTK(KERN_WARNING "Device %s IP: %p4I\n",
                            name, &if_info->ifa_local);
            return if_info->ifa_local;
        }
    }
    MSGPRINTK(KERN_ERR "msg_socket: ERROR - cannot find host ip\n");
    return -1;
}

/* put here since ib request handler and 
 * caller response handler(user defined) will both need.
 */
typedef struct {
	struct pcn_kmsg_hdr header; /* must followd */
	/* rdma essential */
	bool is_rdma;
	bool is_write;
	bool rdma_ack;			/* passive side acks in the end of request */
	uint32_t remote_rkey;	/* R/W remote RKEY (body) */
	uint32_t rw_size;		/* R/W remote size (body) */
	uint64_t remote_addr;	/* remote TO (body) */ 
	void *your_buf_ptr;		/* will be copied to R/W buffer (body) */
#ifdef CONFIG_POPCORN_DEBUG_MSG_LAYER_VERBOSE
	int rw_ticket;
	int rdma_ticket;
#endif	
}__attribute__((packed)) remote_thread_rdma_rw_request_t;

/* Message usage pattern */
#ifdef CONFIG_POPCORN_MSG_STATISTIC
extern struct statistic send_pattern[];
extern struct statistic recv_pattern[];
extern unsigned long g_max_pattrn_size;
extern atomic_t send_cnt;
extern atomic_t recv_cnt;
extern int get_a_slot(struct statistic pattern[], unsigned long size);
#endif

#endif
