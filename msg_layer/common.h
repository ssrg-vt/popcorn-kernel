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
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/kernel.h>
#include <linux/inet.h>
#include <linux/inetdevice.h>
#include <linux/netdevice.h>

#include "config.h"

#define MAX_NUM_NODES		ARRAY_SIZE(ip_addresses)
static uint32_t ip_table[MAX_NUM_NODES] = { 0 };
static uint32_t max_nodes = MAX_NUM_NODES;


static char *ip = "N";
module_param(ip,charp, 0000);
MODULE_PARM_DESC(ip, "");

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
	printk("%s\n",ip);
	if(ip[0]=='N'){
		PCNPRINTK("Loading default node configuration...\n");

		for (i = 0; i < MAX_NUM_NODES; i++) {
			ip_table[i] = in_aton(ip_addresses[i]);
		}
	}
	else{
		PCNPRINTK("Loading user configuration...\n");
		int j, k = 0;
		char* tem, *temp;
/*
		for (i = 0; i < MAX_NUM_NODES; i++) {
			tem = (char*)kmalloc(15*sizeof(char),GFP_KERNEL);
			for(j = 0; j< 16; j++) {
				if( k == strlen(ip)) {i==MAX_NUM_NODES; break;}
				if (ip[k]==':'){ k++;break;} else {tem[j] = ip[k]; k++;}
			}
			
			printk("tem[%d] %s\n",i,tem);
			ip_table[i] = in_aton(tem);
		}
		for (i = 0; i < MAX_NUM_NODES; i++) {
                        printk("%zu\n",ip_table[i]);
                }
*/
		temp=strlen(ip) + ip;
		while (tem = strchrnul(ip, ',')) {
			*tem = 0;
			ip_table[k++] = in_aton(ip);
			ip=tem+1;
			if (ip > temp)
				break;
		}
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
