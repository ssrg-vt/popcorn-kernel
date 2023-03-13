/******************************************************************
 * msg_axi_pcie.c
 * DSM - FPGA/AXI Driver
 * Messaging layer over FPGA interconnect fabric (XDMA + 100G Ethernet Subsystem)
 * Authors: 
 * References: TCP-IP and XDMA messaging layers of Popcorn (Authors: Sang Hoon Kim, Ho-ren Chuang and Naarayanan)
 ******************************************************************/

#define _GNU_SOURCE

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

#define FDSM_MSG_SIZE 8192
#define MAX_RECV_DEPTH 64
#define MAX_SEND_DEPTH	(MAX_RECV_DEPTH)
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

/* BAR Addresses of the FPGA PCIe */

static struct pci_dev *pci_dev;

static char *__pcie_axi_sink_address;
static dma_addr_t __pcie_axi_sink_dma_address;
static struct workqueue_struct *wq;

static struct task_struct *tsk;
static struct task_struct *poll_tsk;

struct semaphore q_empty;
struct semaphore q_full;

/* Index of Receive Queue */

static int page_ix = 0;
static int nid;
static int base_index = 0;

static ktime_t start_s, start_w, end_s, end_w;
s64 actual_time_w, actual_time_s; 

static int KV[XDMA_SLOTS];//array of size 320
u8 __iomem *zynq_hw_addr;

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
    void __iomem *base_addr;
};

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

///Point beginning of the recv queue
/*
static void __update_recv_index(queue_tr *q, int i)
{
	dma_addr_t dma_addr;
	int ret;

	if (i == q->nr_entries)	{
		i = 0;
		q->tail = -1;
	}
	dma_addr = q->work_list[i]->dma_addr;
	//ret = config_descriptors_bypass(dma_addr, FDSM_MSG_SIZE, FROM_DEVICE, KMSG);//addr, 8192, 0, 0
	writeq(dma_addr, zynq_hw_addr+0x10+i);
}
*/
/* Upon completion of a send */
/*
static void __process_sent(struct send_work *work)
{
	if (work->done)
		complete(work->done);
}
*/

/* Enqueue message in send queue - Multi-threaded support.
 * Not required for single-threaded or serial applications.
 */
/*
static void __enq_send(struct send_work *work)
{
	int ret;
	do {

		ret = down_interruptible(&q_full);
		if (ret == -EINTR) {
			return -1;
		}
	} while (ret);

	spin_lock(&send_queue_lock);
	send_queue->tail = (send_queue->tail + 1) % send_queue->nr_entries;
	send_queue->work_list[send_queue->tail] = work;
	send_queue->size++;
	spin_unlock(&send_queue_lock);
	up(&q_empty);
}

static int __get_recv_index(queue_tr *q)
{
	q->tail = (q->tail + 1) % q->nr_entries;
	return q->tail;
}
*/
/* Call popcorn messaging interface process() function */
/*
void process_message(int recv_i)
{
	struct pcn_kmsg_message *msg;
	msg = recv_queue->work_list[recv_i]->addr;

	if (msg->header.type < 0 || msg->header.type >= PCN_KMSG_TYPE_MAX) {
		printk("Need to call a process function\n");
		//pcn_kmsg_pcie_axi_process(PCN_KMSG_TYPE_PROT_PROC_REQUEST, recv_queue->work_list[recv_i]->addr);	
	} else {
		pcn_kmsg_process(msg);
	}
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
*/
/* Global XDMA work pool - DEPRECATED */
/*
static int __refill_pcie_axi_work(int pcie_axi_slot)
{
	int i;
	int nr_refilled = 0;

	struct pcie_axi_work *work_list = NULL;
	struct pcie_axi_work *last_work = NULL;
	for (i = 0; i < pcie_axi_slot; i++) {
		struct pcie_axi_work *xw;

		xw = kzalloc(sizeof(*xw), GFP_KERNEL);
		if (!xw) goto out;

		xw->header.type = WORK_TYPE_XDMA;

		xw->remote_addr = 0;
		xw->dma_addr = 0;
		xw->length = 0;

		if (!last_work) last_work = xw;
		xw->next = work_list;
		work_list = xw;
		nr_refilled++;
	}

out:
	spin_lock(&pcie_axi_work_pool_lock);

	if (work_list) {
		last_work->next = pcie_axi_work_pool;
		pcie_axi_work_pool = work_list;
	}

	spin_unlock(&pcie_axi_work_pool_lock);
	BUG_ON(nr_refilled == 0);
	return nr_refilled;

}

static struct pcie_axi_work *__get_pcie_axi_work(dma_addr_t dma_addr, void *addr, size_t size, dma_addr_t raddr)
{
	struct pcie_axi_work *xw;
	spin_lock(&pcie_axi_work_pool_lock);
	xw = pcie_axi_work_pool;
	pcie_axi_work_pool = pcie_axi_work_pool->next;
	spin_unlock(&pcie_axi_work_pool_lock);
	if (!pcie_axi_work_pool) {

		__refill_pcie_axi_work(XDMA_SLOTS);
	}

	xw->dma_addr = dma_addr;
	xw->addr = addr;
	xw->length = size;
	xw->remote_addr = raddr;
	return xw;
}

static void __put_pcie_axi_work(struct pcie_axi_work *xw)
{
	spin_lock(&pcie_axi_work_pool_lock);
	xw->next = pcie_axi_work_pool;
	pcie_axi_work_pool = xw;
	spin_unlock(&pcie_axi_work_pool_lock);
}
*/
static void __put_pcie_axi_send_work(struct send_work *work)
{	/*
	unsigned long flags;
	if (test_bit(SW_FLAG_MAPPED, &work->flags)) {
		dma_unmap_single(&pci_dev->dev,work->dma_addr, work->length, DMA_TO_DEVICE);
	}

	if (test_bit(SW_FLAG_FROM_BUFFER, &work->flags))	{
		if (unlikely(test_bit(SW_FLAG_MAPPED, &work->flags))) {
			kfree(work->addr);
		} else {
			ring_buffer_put(&pcie_axi_send_buff, work->addr);
		}
	}

	spin_lock_irqsave(&send_work_pool_lock, flags);
	work->next = send_work_pool;
	send_work_pool = work;
	spin_unlock_irqrestore(&send_work_pool_lock, flags);*/
}

/* To send kernel messages to the other node */

int pcie_axi_kmsg_send(int nid, struct pcn_kmsg_message *msg, size_t size)
{	/*
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
			writeq(*(dma_addr_pntr+i), zynq_hw_addr + i);
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
	return ret;*/
}

/* Send messages to remote node */

int pcie_axi_kmsg_post(int nid, struct pcn_kmsg_message *msg, size_t size)
{	/*
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
			writeq(*(dma_addr_pntr+i), zynq_hw_addr + i);
		}
		spin_unlock(&pcie_axi_lock);
	} else {
		printk("DMA addr: not found\n");
	}
	return 0;*/
}


/* To perform of DMA of pages requested by the remote node - DEPRECATED */

int pcie_axi_kmsg_write(int to_nid, dma_addr_t raddr, void *addr, size_t size)
{/*
	//DECLARE_COMPLETION_ONSTACK(done);
	struct pcie_axi_work *xw;
	dma_addr_t dma_addr;
	int ret;
	dma_addr = dma_map_single(&pci_dev->dev,addr, size, DMA_TO_DEVICE);
	ret = dma_mapping_error(&pci_dev->dev,dma_addr);

	if (!((u32)(dma_addr & XDMA_LSB_MASK))) {
		dma_addr = dma_map_single(&pci_dev->dev,addr, size, DMA_TO_DEVICE);
		ret = dma_mapping_error(&pci_dev->dev,dma_addr);
	}

	BUG_ON(ret);
	xw = __get_pcie_axi_work(dma_addr, addr, size, raddr);
	BUG_ON(!xw);
	spin_lock(&pcie_axi_lock);
	ret = config_descriptors_bypass(xw->dma_addr, size, TO_DEVICE, PAGE);
	ret = pcie_axi_transfer(TO_DEVICE);
	spin_unlock(&pcie_axi_lock);

out:
	dma_unmap_single(&pci_dev->dev,dma_addr, size, DMA_TO_DEVICE);
	__put_pcie_axi_work(xw);
	return ret;*/
}

void pcie_axi_kmsg_put(struct pcn_kmsg_message *msg)
{
	/* 
	struct rb_alloc_header *rbah = (struct rb_alloc_header *)msg - 1;
	struct send_work *work = rbah->work;
	__put_pcie_axi_send_work(work); */
}

struct pcn_kmsg_message *pcie_axi_kmsg_get(size_t size)
{	/*
	struct send_work *work = __get_send_work(send_queue->tail);
	return (struct pcn_kmsg_message *)(work->addr);*/
}

int pcie_axi_kmsg_read(int from_nid, void *addr, dma_addr_t raddr, size_t size)
{
	//return -EPERM;
}

void pcie_axi_kmsg_done(struct pcn_kmsg_message *msg)
{
}

void pcie_axi_kmsg_stat(struct seq_file *seq, void *v)
{	/*
	if (seq) {
		seq_printf(seq, POPCORN_STAT_FMT,
			   (unsigned long long)ring_buffer_usage(&pcie_axi_send_buff),
#ifdef CONFIG_POPCORN_STAT
			   (unsigned long long)pcie_axi_send_buff.peak_usage,
#else
			   0ULL,
#endif
			   "Send buffer usage");
	}*/
}

/* Polling KThread Handler */
/*
static int poll_dma(void* arg0)
{	
	//allocate a counter and increment it whenever a message is written

	bool was_frozen;

	//struct pcie_axi_poll_wb *poll_c2h_wb = (struct pcie_axi_poll_wb *)c2h_poll_addr;
	//struct pcie_axi_poll_wb *poll_h2c_wb = (struct pcie_axi_poll_wb *)h2c_poll_addr;
	int counter_rx = *c2h_poll_addr;
	int counter_tx = *h2c_poll_addr;
	u32 c2h_desc_complete = 0;
	u32 h2c_desc_complete = 0;
	int recv_index = 0, index = 0;

	while (!kthread_freezable_should_stop(&was_frozen))	{

		c2h_desc_complete = counter_rx; //poll_c2h_wb->completed_desc_count;
		h2c_desc_complete = counter_tx; //poll_h2c_wb->completed_desc_count;

		if (c2h_desc_complete != 0) {
			//write_register(0x00, (u32 *)(pcie_axi_c + c2h_ctl));
			//write_register(0x06, (u32 *)(pcie_axi_c + c2h_ch));
			//keep polling, zynq updates the counter. Then we can send the recv buffer addr to zynq and zynq sends the data???
			index = __get_recv_index(recv_queue);
			__update_recv_index(recv_queue, index + 1);
			
			recv_index = recv_queue->size;
			//poll_c2h_wb->completed_desc_count = 0;
			c2h_desc_complete = 0;
			recv_queue->size += 1;
			if (recv_queue->size == recv_queue->nr_entries) {
				recv_queue->size = 0;
			}
			process_message(recv_index);
		} else if (h2c_desc_complete != 0) {
			no_of_messages += 1;
			//write_register(0x00, (u32 *)(pcie_axi_c + h2c_ctl));
			//write_register(0x06, (u32 *)(pcie_axi_c + h2c_ch));
			//poll_h2c_wb->completed_desc_count = 0;
			counter_tx = 0;
		}
	}

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
}*/
/*
static void __update_pcie_axi_index(dma_addr_t dma_addr, size_t size)
{
	config_descriptors_bypass(dma_addr, size, FROM_DEVICE, PAGE);
}
*/
/* Buffer handling functions - DEPRECATED */ 
/*
struct pcn_kmsg_pcie_axi_handle *pcie_axi_kmsg_pin_buffer(void *msg, size_t size)
{
	int ret;
	struct pcn_kmsg_pcie_axi_handle *xh = kmalloc(sizeof(*xh), GFP_ATOMIC);
	spin_lock(&__pcie_axi_slots_lock);
	
	xh->addr = __pcie_axi_sink_address + XDMA_SLOT_SIZE * page_ix;
	xh->dma_addr =	__pcie_axi_sink_dma_address + XDMA_SLOT_SIZE * page_ix;
	xh->flags = page_ix;
	KV[page_ix] = 1;
	__update_pcie_axi_index(xh->dma_addr, PAGE_SIZE);
	page_ix += 1;
	spin_unlock(&__pcie_axi_slots_lock);
	return xh;
}*/
/*
void pcie_axi_kmsg_unpin_buffer(struct pcn_kmsg_pcie_axi_handle *handle)
{
	spin_lock(&__pcie_axi_slots_lock);
	BUG_ON(!(KV[handle->flags]));
	KV[handle->flags] = 0;
	spin_unlock(&__pcie_axi_slots_lock);
	kfree(handle);
}*/

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

/*
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
*/
/* Ring Buffer Implementation - DEPRECATED */ 
/*
static __init int __setup_ring_buffer(void)
{
	int ret;
	int i;

	/*Initialize send ring buffer */

//	ret = ring_buffer_init(&pcie_axi_send_buff, "dma_send");
//	if (ret) return ret;
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

/*	for (i = 0; i < MAX_SEND_DEPTH; i++) {
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
			dma_unmap_single(&pci_dev->dev,pcie_axi_send_buff.dma_addr_base[i], RB_CHUNK_SIZE, DMA_TO_DEVICE);
			pcie_axi_send_buff.dma_addr_base[i] = 0;
		}
	}
	return ret;

}
*/
/* Interrupt Handler for monitoring the XDMA reads and writes */
/*
static irqreturn_t pcie_axi_isr(int irq, void *dev_id)
{
	unsigned long read_usr_irq, pkey, addr;
	int ret;

	total += 1;
	read_usr_irq = read_register(pcie_axi_c + usr_irq);

	if (read_usr_irq & 0x01) {
		actual += 1;	
		user_interrupts_disable(KMSG);
		ret = pcie_axi_transfer(FROM_DEVICE);
		user_interrupts_enable(KMSG);
		return IRQ_HANDLED;
	} else if (read_usr_irq & 0x02) {
		user_interrupts_disable(FAULT);
		printk(KERN_ERR "FAULT intr: %x\n", read_usr_irq);
		PCNPRINTK("PKEY: %lx and %lx\n", ioread32((u32 *)(pcie_axi_x + wr_pkey_msb)), ioread32((u32 *)(pcie_axi_x + wr_pkey_lsb)));
		user_interrupts_enable(FAULT);
		return IRQ_HANDLED;
	} else {
		/* Ignore this */
	/*}
	
	return IRQ_HANDLED;
}
*/
/* Polling thread handler initiation */
//Rewrite this function and the poll handles -> poll_dma
/*
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
*/
static void __exit exit_kmsg_pcie_axi(void)
{

	int i;

	/* Detach from messaging layer to avoid race conditions */

	pcn_kmsg_set_transport(NULL);
	//set_popcorn_node_online(nid, false);

	//Unmap the physical address

	//iounmap(pcie_axi_c);
	//iounmap(pcie_axi_x);
	//free_irq(pci_dev->irq, (void *)(pcie_axi_isr));
	/*
	for (i = 0; i < pcie_axi_send_buff.nr_chunks; i++)
	{
		if (pcie_axi_send_buff.dma_addr_base[i]) {
			dma_unmap_single(&pci_dev->dev,pcie_axi_send_buff.dma_addr_base[i], RB_CHUNK_SIZE, DMA_TO_DEVICE);
		}
	}
	*/
	/*
	while (send_work_pool) {
		struct send_work *work = send_work_pool;
		send_work_pool = work->next;
		kfree(work);
	}
	
	ring_buffer_destroy(&pcie_axi_send_buff);
	
	if (domain)
		domain->ops->unmap(domain, base_dma, SZ_2M);
	
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

	dma_free_coherent(&pci_dev->dev, SZ_2M, base_addr, base_dma);
	dma_free_coherent(&pci_dev->dev, 8, c2h_poll_addr, c2h_poll_bus);
	dma_free_coherent(&pci_dev->dev, 8, h2c_poll_addr, h2c_poll_bus);

	if (tsk) {
		wake_up_process(tsk);
	}

	if (poll_tsk) {
		kthread_stop(poll_tsk);
	}
	*/
	PCNPRINTK("Popcorn message layer over AXI unloaded\n");
	return platform_driver_unregister(&axi_pcie_driver);
}

static int axi_pcie_probe(struct platform_device *pdev){
	struct resource *res;
    struct axidma_device *data;
    
    data = devm_kzalloc(&pdev->dev, sizeof(*data), GFP_KERNEL);
    if (!data)
        return -ENOMEM;

    res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
    data->base_addr = devm_ioremap_resource(&pdev->dev, res);
    if (IS_ERR(data->base_addr))
        return PTR_ERR(data->base_addr);

    // Perform hardware initialization
    //writel(0x1, data->base_addr + AXI_CONTROL_REG);

    platform_set_drvdata(pdev, data);
    return 0;
}

static int axidma_remove(struct platform_device *pdev)
{
    struct axidma_device *axidma_dev;

    // Get the AXI DMA device structure from the device's private data
    axidma_dev = dev_get_drvdata(&pdev->dev);

    // Cleanup the character device structures
    //axidma_chrdev_exit(axidma_dev);

    // Cleanup the DMA structures
    //axidma_dma_exit(axidma_dev);

    // Free the device structure
    kfree(axidma_dev);
    return 0;
}

static const struct of_device_id axi_pcie_compatible_of_ids[] = {
    { .compatible = "xlnx,pcie-us-rqrc-1.0" },
    { .compatible = "xlnx,protocol-processor-v1-0-1.0"},
    {}
};

static struct platform_driver axi_pcie_driver = {
	.driver = {
		.name  = MODULE_NAME,
		.owner = THIS_MODULE,
		.of_match_table = axi_pcie_compatible_of_ids,
	},
	.probe  = axi_pcie_probe,
	.remove = axi_pcie_remove,
};

static int __init init_kmsg_pcie_axi(void)
{	
	PCNPRINTK("Popcorn message layer over AXI loaded\n");
	return platform_driver_register(&axi_pcie_driver);
}

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Hemanth Ramesh");
MODULE_DESCRIPTION("AXI_PCIe Messaging Layer");
	
module_param(use_rb_thr, uint, 0644);
MODULE_PARM_DESC(use_rb_thr, "Threshold for using pre-allocated and pre-mapped ring buffer");

module_param_named(features, transport_pcie_axi.features, ulong, 0644);
MODULE_PARM_DESC(use_pcie_axi, "2: FPGA layer to transfer pages");

module_init(init_kmsg_pcie_axi);
module_exit(exit_kmsg_pcie_axi);