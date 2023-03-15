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
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_device.h>

#include "common.h"
#include "ring_buffer.h"

// Local dependencies
//#include "axidma.h"                 // Internal definitions

#define FDSM_MSG_SIZE 8192
#define MAX_RECV_DEPTH 64
#define MAX_SEND_DEPTH  (MAX_RECV_DEPTH)
#define XDMA_SLOT_SIZE PAGE_SIZE * 2
#define XDMA_SLOTS 320 //What is this??

RADIX_TREE(send_tree, GFP_ATOMIC);

static unsigned long *c2h_poll_addr;
static unsigned long *h2c_poll_addr;
static dma_addr_t c2h_poll_bus;
static dma_addr_t h2c_poll_bus;
static void *base_addr; 
static dma_addr_t base_dma;

static struct iommu_domain *domain; 

static unsigned long total = 0;
static unsigned long actual = 0;
static unsigned long no_of_messages = 0;

u64 start_time, end_time, res_time, end_time1, end_time2; 

static unsigned int use_rb_thr = PAGE_SIZE / 2;

static int j = 0;
void __iomem *pcie_axi_x;
void __iomem *pcie_axi_c;
static struct workqueue_struct *wq;

static struct task_struct *tsk;
static struct task_struct *poll_tsk;

struct semaphore q_empty;
struct semaphore q_full;

u8 __iomem *prot_proc_addr;
u8 __iomem *x86_host_addr;

struct work_hdr {
    enum {
        WORK_TYPE_RECV,
        WORK_TYPE_XDMA,
        WORK_TYPE_SEND,
    } type;
};

enum {
    SW_FLAG_MAPPED = 0,
    SW_FLAG_FROM_BUFFER = 1,
};

enum {
    PGREAD = 0,
    PGWRITE = 1,
    VMFC = 2,
    PGINVAL = 3,
    PGRESP = 4,
};

enum {
    KMSG = 0,
    PAGE = 1,
    FAULT = 4,
};

enum {
    AXI = 0,
    CTL = 1,
};

enum {
    FROM_DEVICE = 0,
    TO_DEVICE = 1,
};

/* Index of Receive Queue */

static int page_ix = 0;
static int nid;
static int base_index = 0;

static ktime_t start_s, start_w, end_s, end_w;
s64 actual_time_w, actual_time_s; 
static int KV[XDMA_SLOTS];//array of size 320

/* Send Buffer for pcn_kmsg*/

struct send_work {
    struct work_hdr header;
    void *addr;
    u64 dma_addr;
    u32 length;
    struct send_work *next;
    struct completion *done;
    unsigned long flags;
};

/* Receive Buffer for pcn_kmsg */

struct recv_work {
    struct work_struct work_q;
    struct work_hdr header;
    void *addr;
    u64 dma_addr;
    u32 length;
};

struct pcn_work {
    struct work_struct work_q;
    void *addr;
};

/* XDMA Work Buffer */

struct pcie_axi_work {

    int nid;
    struct work_hdr header;
    struct pcie_axi_work *next;
    u32 length;
    void *addr;
    dma_addr_t dma_addr;
    u64 remote_addr;
    unsigned long flags;
    struct completion *done;

};

struct queue {
    unsigned int tail;
    unsigned int head;
    unsigned int size;
    unsigned int nr_entries;
    struct send_work **work_list;
};

typedef struct queue queue_t;

struct queue_r {
    unsigned int tail;
    unsigned int head;
    unsigned int size;
    unsigned int nr_entries;
    struct recv_work **work_list;
};

typedef struct queue_r queue_tr;


struct rb_alloc_header {
    struct send_work *work;
    unsigned int flags;
    unsigned int magic;

};

/* XDMA polling writeback structure */

struct pcie_axi_poll_wb {
    u32 completed_desc_count;
    u32 reserved_1[7];
} __packed;

struct axidma_device {
   // int num_devices;                // The number of devices
   // unsigned int minor_num;         // The minor number of the device
   // dev_t dev_num;                  // The device number of the device
    char *chrdev_name;              // The name of the character device
   // struct device *device;          // Device structure for the char device
   // struct class *dev_class;        // The device class for the chardevice
    //struct cdev chrdev;             // The character device structure

   // int num_dma_tx_chans;           // The number of transmit DMA channels
   // int num_dma_rx_chans;           // The number of receive DMA channels
   // int num_vdma_tx_chans;          // The number of transmit VDMA channels
   // int num_vdma_rx_chans;          // The number of receive  VDMA channels
   // int num_chans;                  // The total number of DMA channels
   // int notify_signal;              // Signal used to notify transfer completion
    struct platform_device *pdev;   // The platofrm device from the device tree
   // struct axidma_cb_data *cb_data; // The callback data for each channel
   // struct axidma_chan *channels;   // All available channels
   // struct list_head dmabuf_list;   // List of allocated DMA buffers
   // struct list_head external_dmabufs;  // Buffers allocated in other drivers
    void __iomem *base_addr;
};

//struct axidma_device *axidma_dev, *x86_bus, *prot_proc_bus;
//struct device_node *axidma_dev, *x86_bus, *prot_proc_bus;
//struct device *dev;
struct device_node *x86_host, *prot_proc, *parent;
struct resource res1, res2;
unsigned long long x86_host_base_addr, prot_proc_base_addr;
static void *base_addr;
static dma_addr_t base_dma;
struct device_node *x86_host, *prot_proc, *parent;
struct resource res1, res2;
unsigned long long x86_host_base_addr, prot_proc_base_addr;
struct platform_device *pdev;

const unsigned int rb_alloc_header_magic = 0xbad7face;

static DEFINE_SPINLOCK(send_work_pool_lock);
static DEFINE_SPINLOCK(send_queue_lock);
static DEFINE_SPINLOCK(pcie_axi_lock);
static DEFINE_SPINLOCK(__pcie_axi_slots_lock);
static DEFINE_SPINLOCK(pcie_axi_work_pool_lock);

static struct ring_buffer pcie_axi_send_buff = {};
static struct send_work *send_work_pool = NULL;

static queue_t *send_queue;
static queue_tr *recv_queue;

static struct pcie_axi_work *pcie_axi_work_pool = NULL;

/*----------------------------------------------------------------------------
 * Platform Device Functions
 *----------------------------------------------------------------------------*/
static void __update_recv_index(queue_tr *q, int i)
{
    dma_addr_t dma_addr;
    int ret;

    if (i == q->nr_entries) {
        i = 0;
        q->tail = -1;
    }
    dma_addr = q->work_list[i]->dma_addr;
    //ret = config_descriptors_bypass(dma_addr, FDSM_MSG_SIZE, FROM_DEVICE, KMSG);//
    //writeq(dma_addr, zynq_hw_addr+0x10+i);
}

/* Polling KThread Handler */

static int poll_dma(void* arg0)
{   
    bool was_frozen;

    //struct xdma_poll_wb *poll_c2h_wb = (struct xdma_poll_wb *)c2h_poll_addr;
    //struct xdma_poll_wb *poll_h2c_wb = (struct xdma_poll_wb *)h2c_poll_addr;
    int counter_rx = *c2h_poll_addr;
    int counter_tx = *h2c_poll_addr;
    u32 c2h_desc_complete = 0;
    u32 h2c_desc_complete = 0;
    int recv_index = 0, index = 0;

    while (!kthread_freezable_should_stop(&was_frozen)) {

        c2h_desc_complete = poll_c2h_wb->completed_desc_count;
        h2c_desc_complete = poll_h2c_wb->completed_desc_count;

        if (c2h_desc_complete != 0) {
            //write_register(0x00, (u32 *)(xdma_c + c2h_ctl));
            //write_register(0x06, (u32 *)(xdma_c + c2h_ch));
            index = __get_recv_index(recv_queue);
            __update_recv_index(recv_queue, index + 1);
            
            recv_index = recv_queue->size;
            //poll_c2h_wb->completed_desc_count = 0;
            recv_queue->size += 1;
            if (recv_queue->size == recv_queue->nr_entries) {
                recv_queue->size = 0;
            }
            process_message(recv_index);
        } else if (h2c_desc_complete != 0) {
            no_of_messages += 1;
            //write_register(0x00, (u32 *)(xdma_c + h2c_ctl));
            //write_register(0x06, (u32 *)(xdma_c + h2c_ch));
            //poll_h2c_wb->completed_desc_count = 0;
            counter_tx = 0;
        }
    }

    return 0;
}

/* Upon completion of a send */

static void __process_sent(struct send_work *work)
{
    if (work->done)
        complete(work->done);
}

static void __put_pcie_axi_send_work(struct send_work *work)
{
    unsigned long flags;
    if (test_bit(SW_FLAG_MAPPED, &work->flags)) {
        dma_unmap_single(&pci_dev->dev,work->dma_addr, work->length, DMA_TO_DEVICE);
    }

    if (test_bit(SW_FLAG_FROM_BUFFER, &work->flags))    {
        if (unlikely(test_bit(SW_FLAG_MAPPED, &work->flags))) {
            kfree(work->addr);
        } else {
            ring_buffer_put(&pcie_axi_send_buff, work->addr);
        }
    }

    spin_lock_irqsave(&send_work_pool_lock, flags);
    work->next = send_work_pool;
    send_work_pool = work;
    spin_unlock_irqrestore(&send_work_pool_lock, flags);
}

/* Ring Buffer Implementation - DEPRECATED */ 

static __init int __setup_ring_buffer(void)
{
    int ret;
    int i;

    /*Initialize send ring buffer */

    ret = ring_buffer_init(&pcie_axi_send_buff, "dma_send");
    if (ret) return ret;
    /*
    for (i = 0; i < pcie_axi_send_buff.nr_chunks; i++) {
        dma_addr_t dma_addr = dma_map_single(&pci_dev->dev,pcie_axi_send_buff.chunk_start[i], RB_CHUNK_SIZE, DMA_TO_DEVICE);
        ret = dma_mapping_error(&pci_dev->dev,dma_addr);
        printk("ret=%d, addr=%p\n", ret, dma_addr);
        if (ret) goto out_unmap;
        pcie_axi_send_buff.dma_addr_base[i] = dma_addr;
    }
    */
    /* Initialize send work request pool */

    for (i = 0; i < MAX_SEND_DEPTH; i++) {
        struct send_work *work;

        work = kzalloc(sizeof(*work), GFP_KERNEL);
        if (!work) {
            ret = -ENOMEM;
            goto out_unmap;
        }
        work->header.type = WORK_TYPE_SEND;

        work->dma_addr = 0;
        work->length = 0;

        work->next = send_work_pool;
        send_work_pool = work;
    }
    return 0;

out_unmap:
    printk("In out_unmap (ring buffer setup failed).\n");
    while (pcie_axi_work_pool) {
        struct pcie_axi_work *xw = pcie_axi_work_pool;
        pcie_axi_work_pool = xw->next;
        kfree(xw);
    }
    while (send_work_pool) {
        struct send_work *work = send_work_pool;
        send_work_pool = work->next;
        kfree(work);
    }
    for (i = 0; i < pcie_axi_send_buff.nr_chunks; i++) {
        if (pcie_axi_send_buff.dma_addr_base[i]) {
            //dma_unmap_single(&pci_dev->dev,pcie_axi_send_buff.dma_addr_base[i], RB_CHUNK_SIZE, DMA_TO_DEVICE);
            pcie_axi_send_buff.dma_addr_base[i] = 0;
        }
    }
    return ret;

}

static queue_t* __setup_send_queue(int entries)
{
    queue_t* send_q = (queue_t*)kmalloc(sizeof(queue_t), GFP_KERNEL);
    int i, ret;
    if (!send_q) {
        goto out;
    }

    send_q->tail = -1;
    send_q->head = 0;
    send_q->size = 0;
    send_q->nr_entries = entries;
    send_q->work_list = kmalloc(entries * sizeof(struct send_work *), GFP_KERNEL);

    for (i = 0; i<entries; i++) {
        send_q->work_list[i] = kmalloc(sizeof(struct send_work), GFP_KERNEL);
        send_q->work_list[i]->header.type = WORK_TYPE_SEND;
        send_q->work_list[i]->addr = base_addr + FDSM_MSG_SIZE * base_index;//FDSM_MSG_SIZE = 8192
        send_q->work_list[i]->dma_addr = base_dma + FDSM_MSG_SIZE * base_index;
        ++base_index;
        radix_tree_insert(&send_tree, send_q->work_list[i]->addr, send_q->work_list[i]->dma_addr);
    }

    return send_q;

out:
    PCNPRINTK("Send Queue Failed\n");
    return NULL;
}

static __init queue_tr* __setup_recv_buffer(int entries)
{
    queue_tr* recv_q = (queue_tr*)kmalloc(sizeof(queue_tr), GFP_KERNEL);
    int i, index, ret;
    if (!recv_q) {
        goto out;
    }

    recv_q->tail = -1;
    recv_q->head = 0;
    recv_q->size = 0;
    recv_q->nr_entries = entries;
    recv_q->work_list = kmalloc(entries * sizeof(struct recv_work *), GFP_KERNEL);

    for (i = 0; i < entries; i++) {
        recv_q->work_list[i] = kmalloc(sizeof(struct recv_work), GFP_KERNEL);
        recv_q->work_list[i]->header.type = WORK_TYPE_RECV;
        recv_q->work_list[i]->addr = base_addr +  FDSM_MSG_SIZE * base_index;
        recv_q->work_list[i]->dma_addr = base_dma + FDSM_MSG_SIZE * base_index;
        ++base_index;
    }
    __update_recv_index(recv_q, 0);
    return recv_q;

out:
    PCNPRINTK("Receive Queue Setup Failed\n");
    return NULL;
}

/* Polling thread handler initiation */
//Rewrite this function and the poll handles -> poll_dma
static int __start_poll(void)
{   
    poll_tsk = kthread_run(poll_dma, NULL, "Poll_Handler");
    if (IS_ERR(poll_tsk)) {
        PCNPRINTK("Error Instantiating Polling Handler\n");
        return 1;
    }
    
    printk("Start poll.\n");
    return 0;
}

void free_queue(queue_t* q)
{
    int i;
    for (i = 0; i<q->nr_entries; i++) {
        radix_tree_delete(&send_tree, q->work_list[i]->addr);
        kfree(q->work_list[i]);
    }

    kfree(q->work_list);
    kfree(q);
}

void free_queue_r(queue_tr* q)
{
    int i;
    for (i = 0; i< q->nr_entries; i++) {
        kfree(q->work_list[i]);
    }

    kfree(q->work_list);
    kfree(q);
}

static struct send_work *__get_send_work(int index) 
{
    struct send_work *work;
    if (index == send_queue->nr_entries) {
        send_queue->tail = -1;
    }

    spin_lock(&send_queue_lock);
    send_queue->tail = (send_queue->tail + 1) % send_queue->nr_entries;
    work = send_queue->work_list[send_queue->tail]; 
    spin_unlock(&send_queue_lock);

    return work;
}

struct pcn_kmsg_message *pcie_axi_kmsg_get(size_t size)
{
    struct send_work *work = __get_send_work(send_queue->tail);
    return (struct pcn_kmsg_message *)(work->addr);
}

void pcie_axi_kmsg_put(struct pcn_kmsg_message *msg)
{
    /*Nothing here*/
}

void pcie_axi_kmsg_stat(struct seq_file *seq, void *v)
{
    if (seq) {
        seq_printf(seq, POPCORN_STAT_FMT,
               (unsigned long long)ring_buffer_usage(&pcie_axi_send_buff),
#ifdef CONFIG_POPCORN_STAT
               (unsigned long long)pcie_axi_send_buff.peak_usage,
#else
               0ULL,
#endif
               "Send buffer usage");
    }
}

int pcie_axi_kmsg_post(int nid, struct pcn_kmsg_message *msg, size_t size)
{
    int ret, i;
    dma_addr_t dma_addr, *dma_addr_pntr;
    dma_addr = radix_tree_lookup(&send_tree, (unsigned long *)msg);
    dma_addr_pntr = dma_addr;
    if (dma_addr) {
        spin_lock(&pcie_axi_lock);
        //ret = config_descriptors_bypass(dma_addr, FDSM_MSG_SIZE, TO_DEVICE, KMSG);
        //ret = pcie_axi_transfer(TO_DEVICE);
        //get the recv buffer addr and write data there.
        for(i=0; i<FDSM_MSG_SIZE; i++){
            writeq(*(dma_addr_pntr+i), x86_host_addr + i);
        }
        spin_unlock(&pcie_axi_lock);
    } else {
        printk("DMA addr: not found\n");
    }
    return 0;
}

int pcie_axi_kmsg_send(int nid, struct pcn_kmsg_message *msg, size_t size)
{
    struct send_work *work;
    int ret, i;
    u64 *dma_addr_pntr;
    DECLARE_COMPLETION_ONSTACK(done);

    work = __get_send_work(send_queue->tail);

    memcpy(work->addr, msg, size);

    work->done = &done;
    spin_lock(&pcie_axi_lock);
    //ret = config_descriptors_bypass(work->dma_addr, FDSM_MSG_SIZE, TO_DEVICE, KMSG);
    //ret = pcie_axi_transfer(TO_DEVICE);
    dma_addr_pntr = work->dma_addr;
    
    for(i=0; i<FDSM_MSG_SIZE; i++){
            writeq(*(dma_addr_pntr+i), x86_host_addr + i);
        }
    spin_unlock(&pcie_axi_lock);
    
    __process_sent(work);
    if (!try_wait_for_completion(&done)){
        ret = wait_for_completion_io_timeout(&done, 60 *HZ);
        if (!ret) {
            printk("Message waiting failed\n");
            ret = -ETIME;
            goto out;
        }
    }
    return 0;

out:
    __put_pcie_axi_send_work(work);
    return ret;
}

void pcie_axi_kmsg_done(struct pcn_kmsg_message *msg)
{
    /*Nothing here*/
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

static void __exit axidma_exit(void)
{   
    int i;

    /* Detach from messaging layer to avoid race conditions */

    pcn_kmsg_set_transport(NULL);
    set_popcorn_node_online(nid, false);

    iounmap(x86_host_addr);
    iounmap(prot_proc_addr);

    dma_free_coherent(&pdev->dev, SZ_2M, base_addr, base_dma);

    while (send_work_pool) {
        struct send_work *work = send_work_pool;
        send_work_pool = work->next;
        kfree(work);
    }
    
    ring_buffer_destroy(&pcie_axi_send_buff);

    if (send_queue)
        free_queue(send_queue);
    
    if (recv_queue)
        free_queue_r(recv_queue);

    while (pcie_axi_work_pool) {
        struct pcie_axi_work *xw = pcie_axi_work_pool;
        pcie_axi_work_pool = xw->next;
        kfree(xw);
    }

    destroy_workqueue(wq);

    
    dma_free_coherent(&pdev->dev, 8, c2h_poll_addr, c2h_poll_bus);
    dma_free_coherent(&pdev->dev, 8, h2c_poll_addr, h2c_poll_bus);

    if (tsk) {
        wake_up_process(tsk);
    }

    if (poll_tsk) {
        kthread_stop(poll_tsk);
    }
    printk("Unloaded axi module\n");
    //return platform_driver_unregister(&axidma_driver);
}

static int __init axidma_init(void)
{
    int ret, size;
           
    PCNPRINTK("Initializing module over AXI\n");
    pcn_kmsg_set_transport(&transport_pcie_axi);
    PCNPRINTK("registered transport layer\n");

    parent = of_find_node_by_name(NULL, "amba_pl");

    // Find the "pcie_us_rqrc_1" child node
    x86_host = of_find_node_by_name(parent, "pcie_us_rqrc");
    if (x86_host) {
        if (of_address_to_resource(x86_host, 0, &res1) == 0) {
            x86_host_base_addr = (unsigned long long)res1.start;
            pr_info("pcie_us_rqrc base address = 0x%llx\n", x86_host_base_addr);
            x86_host_addr = ioremap(x86_host_base_addr, resource_size(&res1));
            if(!x86_host_addr)
                ret = -ENOMEM;
        }
        of_node_put(x86_host);
    }

    // Find the "protocol_processor_v_2" child node
    prot_proc = of_find_node_by_name(parent, "protocol_processor_v1_0");
    if (prot_proc) {
        if (of_address_to_resource(prot_proc, 0, &res2) == 0) {
            prot_proc_base_addr = (unsigned long long)res2.start;
            pr_info("protocol_processor_v1_0 base address = 0x%llx\n", prot_proc_base_addr);
            prot_proc_addr = ioremap(prot_proc_base_addr, resource_size(&res2));
            if(!prot_proc_addr)
                ret = -ENOMEM;
        }
        of_node_put(prot_proc);
    }

    writeq(0x1234567812345678, x86_host_addr);
    printk("Readq = %llx\n",readq(x86_host_addr));

    writeq(0xabcdef01abcdef01, prot_proc_addr);
    printk("Readq = %llx\n",readq(prot_proc_addr));
    
    my_nid = 1;
    //Write the node ID to the protocol processor
    iowrite32(0x1, prot_proc_addr+0x34);
    printk("prot_proc_addr=%p\n",ioread32(prot_proc_addr+0x34));
    set_popcorn_node_online(my_nid, true);

    pdev = of_find_device_by_node(x86_host);
    base_addr = dma_alloc_coherent(&pdev->dev, SZ_2M, &base_dma, GFP_KERNEL);
/*
#ifdef CONFIG_ARM64 
        domain = iommu_get_domain_for_dev(&pdev->dev);
        if (!domain) goto out_free;
    
        ret = domain->ops->map(domain, base_dma, virt_to_phys(base_addr), SZ_2M, IOMMU_READ | IOMMU_WRITE);
#endif
*/
    if (__setup_ring_buffer())
        goto out_free;
    printk("Ring buffer setup complete.\n");

    wq = create_workqueue("recv");
    if (!wq)
        goto out_free;
    printk("Work queue created.\n");

    send_queue = __setup_send_queue(MAX_SEND_DEPTH);//64
    if (!send_queue) 
        goto out_free;
    printk("Send queue setup completed.\n");

    //Other node must know this address
    recv_queue = __setup_recv_buffer(MAX_RECV_DEPTH);//64
    if (!recv_queue)
        goto out_free;
    printk("Receive buffer setup completed.\n");

    //memset(KV, 0, XDMA_SLOTS * sizeof(int)); //320*4bytes = 1280 bytes
    sema_init(&q_empty, 0);
    sema_init(&q_full, MAX_SEND_DEPTH);

    //Allocate 8byte of memory for the counter where c2h_poll_bus and h2c_poll_bus are the 
    //address of the counters. 
    c2h_poll_addr = dma_alloc_coherent(&pdev->dev, 8, &c2h_poll_bus, GFP_KERNEL);
    h2c_poll_addr = dma_alloc_coherent(&pdev->dev, 8, &h2c_poll_bus, GFP_KERNEL);
/*
#ifdef CONFIG_ARM64
        ret = domain->ops->map(domain, (unsigned long)h2c_poll_bus, virt_to_phys(h2c_poll_addr), PAGE_SIZE, IOMMU_READ | IOMMU_WRITE);
        if (ret) goto out_free;
            ret = domain->ops->map(domain, (unsigned long)c2h_poll_bus, virt_to_phys(c2h_poll_addr), PAGE_SIZE, IOMMU_READ | IOMMU_WRITE);
        if (ret) goto out_free;
#endif
*/
    writeq(c2h_poll_addr, x86_host_addr);
    printk("c2h_poll_addr=%llx", readq(x86_host_addr));
    writeq(h2c_poll_addr, x86_host_addr+0x08);
    printk("c2h_poll_addr=%llx", readq(x86_host_addr+0x08));

    if (__start_poll()) 
        goto out_free;

    broadcast_my_node_info(2);
    PCNPRINTK("... Ready on AXI ... \n");

    return 0;

out:
    PCNPRINTK("PCIe Device not found!!\n");
    axidma_exit();  
    return -EINVAL;

invalid:
    PCNPRINTK("DMA Bypass not found!..\n");
    axidma_exit();
    return -EINVAL;

out_free:
    PCNPRINTK("Inside Out Free of INIT\n");
    axidma_exit();
    return -EINVAL;
}

module_init(axidma_init);
module_exit(axidma_exit);

MODULE_AUTHOR("XYZ");


MODULE_LICENSE("GPL");
MODULE_VERSION("1.0");
MODULE_DESCRIPTION("Module to provide a userspace interface for transferring "
                   "data from the processor to the logic fabric via AXI DMA.");
