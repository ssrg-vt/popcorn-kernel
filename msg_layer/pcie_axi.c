/*Restore to this commit c4d049a74951ae025c36dbfa0e3ffc3b9c5db543 if something goes wrong*/
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
#include <linux/io.h>
#include <linux/rcupdate.h>
#include <asm/barrier.h>

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

static void *base_addr; 
static dma_addr_t base_dma;
//static int send_count=1, post_count=1, poll_count=1;

static unsigned long no_of_messages = 0;
u64 start_time, end_time, res_time, end_time1, end_time2; 
static unsigned int use_rb_thr = PAGE_SIZE / 2;

void __iomem *pcie_axi_x;
void __iomem *pcie_axi_c;
static struct workqueue_struct *wq;

static struct task_struct *poll_tsk;
struct semaphore sem;
static struct pci_dev *pci_dev;
u8 __iomem *prot_proc_addr;
u8 __iomem *zynq_hw_addr;

resource_size_t zynq_hw_regs_size;
phys_addr_t zynq_hw_regs_phys;
u32 h2c_desc_complete = 0;

#define NODE_INFO_FIELDS \
    int nid; \
    int bundle_id; \
    int arch;
DEFINE_PCN_KMSG(node_info_t, NODE_INFO_FIELDS);

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

static int nid;
static int base_index = 0;
s64 actual_time_w, actual_time_s; 

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

static void *base_addr;
static dma_addr_t base_dma;

const unsigned int rb_alloc_header_magic = 0xbad7face;

static DEFINE_SPINLOCK(send_work_pool_lock);
static DEFINE_SPINLOCK(send_queue_lock);
static DEFINE_SPINLOCK(pcie_axi_lock);

static struct ring_buffer pcie_axi_send_buff = {};
static struct send_work *send_work_pool = NULL;

static queue_t *send_queue;
static queue_tr *recv_queue;

/*----------------------------------------------------------------------------
 * Platform Device Functions
 *----------------------------------------------------------------------------*/
static void __update_recv_index(queue_tr *q, int i)
{   
    if (i == q->nr_entries) {
        i = 0;
        q->tail = -1;
    }
    writeq(0x00000000fefefefe, zynq_hw_addr); //Reset the physical address
    writeq(q->work_list[i]->dma_addr, zynq_hw_addr); //Update the physical address with next sector address of recv Q
}

static int __get_recv_index(queue_tr *q)
{   
    q->tail = (q->tail + 1) % q->nr_entries;
    return q->tail;
}

/* Call popcorn messaging interface process() function */

void process_message(int recv_i)
{
    struct pcn_kmsg_message *msg;
    msg = recv_queue->work_list[recv_i]->addr;
    pcn_kmsg_process(msg);
    /*
    printk("process messgage in %llx\n", virt_to_phys(recv_queue->work_list[recv_i]->addr));
    if (msg->header.type < 0 || msg->header.type >= PCN_KMSG_TYPE_MAX) {
        printk("Need to call a process function\n");
        //pcn_kmsg_pcie_axi_process(PCN_KMSG_TYPE_PROT_PROC_REQUEST, recv_queue->work_list[recv_i]->addr);  
    } else {
        printk("ARMs nid = %d\n", msg->header.from_nid);
        node_info_t *info = (node_info_t *)msg;
        printk("ARMs nid from nid_info=%d\n", info->nid);
        printk("The msessage is %llx\n", *(uint64_t *)msg);
        pcn_kmsg_process(msg);
    }*/
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

/* Polling KThread Handler */

static int poll_dma(void* arg0)
{   
    bool was_frozen;
    int recv_index = 0, index = 0, tmp = 0;
    //printk("In poll_dma\n");
    while (!kthread_freezable_should_stop(&was_frozen)) {

        if (*(uint64_t *)((recv_queue->work_list[tmp]->addr)+(1023*8)) == 0xd010d010) {
            
            *(uint64_t *)((recv_queue->work_list[tmp]->addr)+(1023*8)) = 0x0;
            tmp = (tmp+1)%64;
            //printk("tmp = %d\n", tmp);
            index = __get_recv_index(recv_queue);
            //printk("Index = %d\n", index);
            __update_recv_index(recv_queue, index+1);
            //printk("Poll count = %d\n", poll_count);
            //poll_count++;
            recv_index = recv_queue->size;
            recv_queue->size += 1;
            if (recv_queue->size == recv_queue->nr_entries) {
                recv_queue->size = 0;
            }
            process_message(recv_index);
            //printk("Processed popcorn message.\n");
        } else if (h2c_desc_complete != 0) {
            no_of_messages += 1;
            h2c_desc_complete = 0;
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

static queue_t* __setup_send_queue(int entries)
{
    queue_t* send_q = (queue_t*)kmalloc(sizeof(queue_t), GFP_KERNEL);
    int i;
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
        radix_tree_insert(&send_tree, (unsigned long)(send_q->work_list[i]->addr), send_q->work_list[i]->addr);//send_q->work_list[i]->dma_addr); Inserting the msg as key and storing the address. 
        //printk("key=%llx value=%llx\n",(unsigned long)send_q->work_list[i]->addr, (send_q->work_list[i]->addr));
    }

    return send_q;

out:
    PCNPRINTK("Send Queue Failed\n");
    return NULL;
}

static __init queue_tr* __setup_recv_buffer(int entries)
{
    queue_tr* recv_q = (queue_tr*)kmalloc(sizeof(queue_tr), GFP_KERNEL);
    int i;
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
        //printk("Recv Q addr=%llx\n",(recv_q->work_list[i]->dma_addr));
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
        radix_tree_delete(&send_tree, (unsigned long)(q->work_list[i]->addr));
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

static int __config_pcie(struct pci_dev *dev)
{
    int ret;
    pci_dev_put(pci_dev);

    ret = pci_enable_device(pci_dev);
    if (ret) return ret;

    return 0;
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
    //printk("In post\n");
    int i;
    if (radix_tree_lookup(&send_tree, (unsigned long)((unsigned long *)msg))) {

        spin_lock(&pcie_axi_lock);
        printk("In post\n");
        for(i=0; i<((FDSM_MSG_SIZE/8)-2); i++){
                //Tried memory barrier, doesn't seem to work.
                //The issue could be due to the buffer capacity in the PCIE IP, which could over get filled up. 
                __raw_writeq(*(u64 *)(radix_tree_lookup(&send_tree, (unsigned long)((unsigned long *)msg))+(i*8)), (zynq_hw_addr + (i*8)));
                //__iowrite64_copy((zynq_hw_addr + (i*8)), (u64 *)(radix_tree_lookup(&send_tree, (unsigned long)((unsigned long *)msg))+(i*8)), 1);
                udelay(2); //Added an explicit delay (NOT RECOMMENDED!!!), seem to work. Need to think of a better approach
                //printk("Data in post=%llx\n",*(u64 *)(radix_tree_lookup(&send_tree, (unsigned long)((unsigned long *)msg))+(i*8)));
        }
        __raw_writeq(0x00000000d010d010, (zynq_hw_addr+(1022*8)));
        __raw_writeq(0x00000000d010d010, (zynq_hw_addr+(1023*8)));
        spin_unlock(&pcie_axi_lock);
        h2c_desc_complete = 1;
    } else {
        printk("DMA addr: not found\n");
    }
    return 0;
}

int pcie_axi_kmsg_send(int nid, struct pcn_kmsg_message *msg, size_t size)//0,
{   
    struct send_work *work;
    int ret, i;
    //printk("In pcie_axi_kmsg_send\n");
    DECLARE_COMPLETION_ONSTACK(done);

    work = __get_send_work(send_queue->tail);

    memcpy(work->addr, msg, size);

    work->done = &done;
    spin_lock(&pcie_axi_lock);
    printk("In send\n");
    for(i=0; i<((FDSM_MSG_SIZE/8)-2); i++){ 
            //Tried memory barrier, doesn't seem to work.
            //The issue could be due to the buffer capacity in the PCIE IP, which could over get filled up. 
            __raw_writeq(*(u64 *)((work->addr)+(i*8)), (zynq_hw_addr+(i*8)));
            //__iowrite64_copy((zynq_hw_addr+(i*8)), (u64 *)((work->addr)+(i*8)), 1);
            udelay(2);//Added an explicit delay (NOT RECOMMENDED!!!), seem to work. Need to think of a better approach
            //printk("Data in send=%llx\n",*(u64 *)((work->addr)+(i*8)));

        }
    __raw_writeq(0x00000000d010d010, zynq_hw_addr+(1022*8)); //Write the last 2 bytes with a patter to indicate the polling thread.
    __raw_writeq(0x00000000d010d010, zynq_hw_addr+(1023*8));    
    spin_unlock(&pcie_axi_lock);
    h2c_desc_complete = 1;
    
    __process_sent(work);
    printk("After process sent\n");
    if (!try_wait_for_completion(&done)){
        printk("After try_wait\n");
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
    /* Detach from messaging layer to avoid race conditions */

    pcn_kmsg_set_transport(NULL);
    iounmap(zynq_hw_addr);
    set_popcorn_node_online(nid, false);
    dma_free_coherent(&pci_dev->dev, SZ_2M, base_addr, base_dma);

    destroy_workqueue(wq);

    if (send_queue)
        free_queue(send_queue);
    
    if (recv_queue)
        free_queue_r(recv_queue);

    if (poll_tsk) {
        kthread_stop(poll_tsk);
    }
    printk("Unloaded pcie module\n");
}

static int __init axidma_init(void)
{
    int ret;
    PCNPRINTK("Initializing module over PCIe\n");
    pcn_kmsg_set_transport(&transport_pcie_axi);
    PCNPRINTK("registered transport layer\n");

    pci_dev = pci_get_device(0x1234, 0x1001, NULL);
    if (pci_dev == NULL) goto out;

    ret = pci_set_dma_mask(pci_dev, DMA_BIT_MASK(32));
    dma_set_mask_and_coherent(&pci_dev->dev, DMA_BIT_MASK(32));

    ret =__config_pcie(pci_dev);
    if (ret){
        goto invalid;
    }

    zynq_hw_regs_size = pci_resource_len(pci_dev, 2);
    zynq_hw_regs_phys = pci_resource_start(pci_dev, 2);

    if (zynq_hw_regs_size) {
        zynq_hw_addr = pci_ioremap_bar(pci_dev, 2);
        if (!zynq_hw_addr) {
            ret = -ENOMEM;
            goto out;
        }
        printk("Zynq address = %llx\n", zynq_hw_addr);
    }

    printk("Before PCIe init\n");
    init_pcie_xdma(pci_dev, zynq_hw_addr);

    /*Need to include a function that passes the zynq_hw_addr to the kernel for prot proc requests.*/

    my_nid = 0;
    set_popcorn_node_online(my_nid, true);
    pci_set_master(pci_dev);
    base_addr = dma_alloc_coherent(&pci_dev->dev, SZ_2M, &base_dma, GFP_KERNEL);//2 x 64 regions x 8KB
    if(!base_addr){
        goto out_free;
    }

    wq = create_workqueue("recv");
    if (!wq)
        goto out_free;
    PCNPRINTK("Work queue created.\n");

    send_queue = __setup_send_queue(MAX_SEND_DEPTH);//64
    if (!send_queue) 
        goto out_free;
    PCNPRINTK("Send queue setup completed.\n");

    //Other node must know this address
    recv_queue = __setup_recv_buffer(MAX_RECV_DEPTH);//64
    if (!recv_queue)
        goto out_free;
    PCNPRINTK("Receive buffer setup completed.\n");

    //memset(KV, 0, XDMA_SLOTS * sizeof(int)); //320*4bytes = 1280 bytes
    sema_init(&sem, 1);

    if (__start_poll()) 
        goto out_free;

    broadcast_my_node_info(2);
    PCNPRINTK("... Ready on PCIe ... \n");

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

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Hemanth Ramesh");
MODULE_DESCRIPTION("PCIe-AXI Messaging Layer");

module_param(use_rb_thr, uint, 0644);
MODULE_PARM_DESC(use_rb_thr, "Threshold for using pre-allocated and pre-mapped ring buffer");

module_param_named(features, transport_pcie_axi.features, ulong, 0644);
MODULE_PARM_DESC(use_pcie, "2: FPGA layer to transfer pages");

module_init(axidma_init);
module_exit(axidma_exit);