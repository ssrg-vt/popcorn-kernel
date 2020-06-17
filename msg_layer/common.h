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

#define CONFIG_FILE_LEN 256
#define CONFIG_FILE_PATH    "/etc/popcorn/nodes"
#define CONFIG_FILE_CHUNK_SIZE  512

static uint32_t ip_table[MAX_NUM_NODES] = { 0 };
static uint32_t max_nodes = MAX_NUM_NODES;

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

	my_ip = __get_host_ip();

	for (i = 0; i < max_nodes; i++) {
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

static bool load_config_file(char *file)
{
	struct file *fp;
	int bytes_read, ret;
	int num_nodes = 0;
	bool retval = true;
	char ip_addr[CONFIG_FILE_CHUNK_SIZE];
	u8 i4_addr[4];
	loff_t offset = 0;
	const char *end;

	/* If no path was passed in, use hard coded default */
	if (file[0] == '\0') {
		strlcpy(file, CONFIG_FILE_PATH, CONFIG_FILE_LEN);
	}

	fp = filp_open(file, O_RDONLY, 0);
	if (IS_ERR(fp)) {
		MSGPRINTK("Cannot open config file %ld\n", PTR_ERR(fp));
		return false;
	}

	while (num_nodes < (max_nodes - 1)) {
		bytes_read = kernel_read(fp, offset, ip_addr, CONFIG_FILE_CHUNK_SIZE);
		if (bytes_read > 0) {
			int str_off, str_len, j;

			/* Replace \n, \r with \0 */
			for (j = 0; j < CONFIG_FILE_CHUNK_SIZE; j++) {
				if (ip_addr[j] == '\n' || ip_addr[j] == '\r') {
					ip_addr[j] = '\0';
				}
			}

			str_off = 0;
			str_len = strlen(ip_addr);
			while (str_off < bytes_read) {
				str_len = strlen(ip_addr + str_off);

				/* Make sure IP address is a valid IPv4 address */
				if(str_len > 0){
					ret = in4_pton(ip_addr + str_off, -1, i4_addr, -1, &end);
					if (!ret) {
						MSGPRINTK("invalid IP address in config file\n");
						retval = false;
						goto done;
					}

					ip_table[num_nodes++] = *((uint32_t *) i4_addr);
				}

				str_off += str_len + 1;
			}
		} else {
			break;
		}
	}

	/* Update max_nodes with number of nodes read in from config file */
	max_nodes = num_nodes;

done:
	filp_close(fp, NULL);
	return retval;
}
#endif
