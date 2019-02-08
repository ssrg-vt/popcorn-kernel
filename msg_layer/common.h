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

#include <linux/inet.h>
#include <linux/inetdevice.h>
#include <linux/netdevice.h>

#include "config.h"

#define MAX_NUM_NODES		ARRAY_SIZE(ip_addresses)
#define MAX_CONN_PER_NODE	1
#if (MAX_CONN_PER_NODE > 1)
#define MULTI_MSG_CANNEL_PER_NODE 1
#else
#define MULTI_MSG_CANNEL_PER_NODE 0
#endif

static uint32_t ip_table[MAX_NUM_NODES] = { 0 };

static uint32_t __init __get_host_ip(void)
{
	struct net_device *d;
	for_each_netdev(&init_net, d) {
		struct in_ifaddr *ifaddr;

		for (ifaddr = d->ip_ptr->ifa_list; ifaddr; ifaddr = ifaddr->ifa_next) {
			int i;
			uint32_t addr = ifaddr->ifa_local;
			for (i = 0; i < MAX_NUM_NODES; i++) {
				if (addr == ip_table[i]) {
					return addr;
				}
			}
		}
	}
	return -1;
}

bool __init identify_myself(void)
{
	int i;
	uint32_t my_ip;

	PCNPRINTK("Loading node configuration...");

	for (i = 0; i < MAX_NUM_NODES; i++) {
		ip_table[i] = in_aton(ip_addresses[i]);
	}

	my_ip = __get_host_ip();

	for (i = 0; i < MAX_NUM_NODES; i++) {
		char *me = " ";
		if (my_ip == ip_table[i]) {
			my_nid = i;
			me = "*";
		}
		PCNPRINTK("%s %d: %pI4\n", me, i, ip_table + i);
	}

	if (my_nid < 0) {
		PCNPRINTK_ERR("My IP is not listed in the node configuration\n");
		return false;
	}

	return true;
}
#endif
