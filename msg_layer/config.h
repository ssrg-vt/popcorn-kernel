#ifndef __POPCORN_MSG_LAYER_CONFIG_H__
#define __POPCORN_MSG_LAYER_CONFIG_H__

#include <linux/net.h>
#include <linux/inet.h>
#include <linux/inetdevice.h>

/**
 * XXX DO NOT commit your local file
 */

const char *ip_addresses[] = {
	/* ib */
	/*
	"192.168.69.129",	// echo5 ib0
	"192.168.69.128",	// echo6 ib0
	*/

	/* dolphin */
	/*
    10.4.4.15",
    10.4.4.16",
	*/

	/* socket */
    "10.0.0.100",
    "10.0.0.20",
    "10.0.0.101",
};

#define MAX_NUM_NODES		(sizeof(ip_addresses) \
								/ sizeof(typeof(*ip_addresses)))
#define MAX_NUM_CHANNELS	(MAX_NUM_NODES - 1)

const char *net_dev_names[] = {
    "ib0",      // InfiniBand
    "eth0",     // Socket/dolphin
    "br0",      // bridge
    "p7p1",     // Xgene (ARM)
};

static uint32_t ip_table[MAX_NUM_NODES] = { 0 };


/**
 * sanghoon: These functions are in the header file due to the conflict in
 * object inclusion in the Makefile
 */

static uint32_t __get_host_ip(const char **name_ret)
{
    int i;
    for (i = 0; i < sizeof(net_dev_names) / sizeof(*net_dev_names); i++) {
		struct net_device *device;
        struct in_device *in_dev;
        const char *name = net_dev_names[i];
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

bool init_ip_table(void)
{
	int i;
	const char *name;
	const uint32_t my_ip = __get_host_ip(&name);

	for(i = 0; i< MAX_NUM_NODES; i++) {
		char *me = " ";
		ip_table[i] = in_aton(ip_addresses[i]);
	   	if (my_ip == ip_table[i]) {
			my_nid = i;
			me = "*";
			smp_mb();
		}
		PRINTK(" %s %2d: %pI4\n", me, i, ip_table + i);
    }
	PRINTK("\n");

	if (my_nid < 0) {
		PRINTK(" No my IP in the node configuration\n");
		return false;
	}

	return true;
}

#endif
