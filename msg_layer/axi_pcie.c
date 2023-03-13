/**
 * @file axi_dma.c
 * @date Saturday, November 14, 2015 at 11:20:00 AM EST
 * @author Brandon Perez (bmperez)
 * @author Jared Choi (jaewonch)
 *
 * This file contains the top level functions for AXI DMA module.
 *
 * @bug No known bugs.
 **/

// Kernel dependencies
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <asm/pci.h>
#include <linux/spinlock.h>
#include <linux/kthread.h>
#include <linux/iommu.h>
#include <asm/io.h>
#include <linux/mm.h>
#include <linux/dma-mapping.h>
#include <linux/errno.h>
#include <linux/seq_file.h>
#include <linux/workqueue.h>
#include <linux/delay.h>
#include <linux/time.h>
#include <linux/timekeeping.h>
#include <linux/radix-tree.h>
#include <popcorn/stat.h>
#include <popcorn/pcn_kmsg.h>
#include <popcorn/page_server.h>

#include "common.h"
#include "ring_buffer.h"

// Local dependencies
//#include "axidma.h"                 // Internal definitions

#define PROTOCOL_PROCESSOR 0xb0000000
#define X86_HOST 0xa0000000
/*----------------------------------------------------------------------------
 * Module Parameters
 *----------------------------------------------------------------------------*/
/*
struct axidma_device {
    int num_devices;                // The number of devices
    unsigned int minor_num;         // The minor number of the device
    dev_t dev_num;                  // The device number of the device
    char *chrdev_name;              // The name of the character device
    struct device *device;          // Device structure for the char device
    struct class *dev_class;        // The device class for the chardevice
    struct cdev chrdev;             // The character device structure

    int num_dma_tx_chans;           // The number of transmit DMA channels
    int num_dma_rx_chans;           // The number of receive DMA channels
    int num_vdma_tx_chans;          // The number of transmit VDMA channels
    int num_vdma_rx_chans;          // The number of receive  VDMA channels
    int num_chans;                  // The total number of DMA channels
    int notify_signal;              // Signal used to notify transfer completion
    struct platform_device *pdev;   // The platofrm device from the device tree
    struct axidma_cb_data *cb_data; // The callback data for each channel
    struct axidma_chan *channels;   // All available channels
    struct list_head dmabuf_list;   // List of allocated DMA buffers
    struct list_head external_dmabufs;  // Buffers allocated in other drivers
};*/

u8 __iomem *prot_proc_addr;
u8 __iomem *x86_host_addr;
/*----------------------------------------------------------------------------
 * Platform Device Functions
 *----------------------------------------------------------------------------*/
struct pcn_kmsg_message *pcie_axi_kmsg_get(size_t size)
{
}

void pcie_axi_kmsg_put(struct pcn_kmsg_message *msg)
{
}

void pcie_axi_kmsg_stat(struct seq_file *seq, void *v)
{
}

int pcie_axi_kmsg_post(int nid, struct pcn_kmsg_message *msg, size_t size)
{
}

int pcie_axi_kmsg_send(int nid, struct pcn_kmsg_message *msg, size_t size)
{
}

void pcie_axi_kmsg_done(struct pcn_kmsg_message *msg)
{
}

struct pcn_kmsg_transport transport_pcie_axi = {
    .name = "axi_pcie",
    .features = 2,//PCN_KMSG_FEATURE_XDMA,

    .get = pcie_axi_kmsg_get,
    .put = pcie_axi_kmsg_put,
    .stat = pcie_axi_kmsg_stat,

    .post = pcie_axi_kmsg_post,
    .send = pcie_axi_kmsg_send,
    .done = pcie_axi_kmsg_done,

    //.pin_pcie_axi_buffer = pcie_axi_kmsg_pin_buffer,
    //.unpin_pcie_axi_buffer = pcie_axi_kmsg_unpin_buffer,
    //.pcie_axi_write = pcie_axi_kmsg_write,
    //.pcie_axi_read = pcie_axi_kmsg_read,

};

/*----------------------------------------------------------------------------
 * Module Initialization and Exit
 *----------------------------------------------------------------------------*/

static int __init axidma_init(void)
{
    PCNPRINTK("Initializing module over AXI\n");
    pcn_kmsg_set_transport(&transport_pcie_axi);
 
    x86_host_addr = ioremap(X86_HOST, 0x1000000);
    prot_proc_addr = ioremap(PROTOCOL_PROCESSOR, 0x10000);

    printk("x86_host_addr=%p\n",x86_host_addr);
    printk("prot_proc_addr=%p\n",prot_proc_addr);

    writeq(0x1234567812345678, x86_host_addr);
    printk("Readq = %lld\n",readq(x86_host_addr));

    writeq(0xabcdef01abcdef01, prot_proc_addr);
    printk("Readq = %lld\n",readq(prot_proc_addr));

    return 0;
}

static void __exit axidma_exit(void)
{
    iounmap(x86_host_addr);
    iounmap(prot_proc_addr);
    printk("Unloaded axi module\n");
    return;
}

module_init(axidma_init);
module_exit(axidma_exit);

MODULE_AUTHOR("XYZ");


MODULE_LICENSE("GPL");
MODULE_VERSION("1.0");
MODULE_DESCRIPTION("Module to provide a userspace interface for transferring "
                   "data from the processor to the logic fabric via AXI DMA.");
