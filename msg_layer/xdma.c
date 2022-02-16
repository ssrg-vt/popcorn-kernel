/******************************************************************
 * msg_xdma.c
 * fDSM - FPGA/PCIe Driver
 * Messaging layer over FPGA interconnect fabric (XDMA + 100G Ethernet Subsystem)
 * Authors: Naarayanan Rao VS <naarayananrao@vt.edu>
 * References: TCP-IP and RDMA messaging layers of Popcorn (Authors: Sang Hoon Kim, Ho-ren Chuang)
 ******************************************************************/

/*
 * NOTE: Some functions are deprecated/inactive in this driver. 
 * We have retained them to help future developers to build upon this implementation using the Popcorn infrastructure. 
 * Regardless, to understand the active functions please go through the Popcorn kernel page server code.
 */

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
#include <popcorn/pcie.h>

#include "common.h"
#include "ring_buffer.h"

#define MAX_RECV_DEPTH 64
#define MAX_SEND_DEPTH	(MAX_RECV_DEPTH)
#define XDMA_SLOT_SIZE PAGE_SIZE * 2
#define XDMA_SLOTS 320 

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

unsigned long ctl_address;
unsigned long axi_address;
static int j = 0;
void __iomem *xdma_x;
void __iomem *xdma_c;

/* BAR Addresses of the FPGA PCIe */

static struct pci_dev *pci_dev;

static char *__xdma_sink_address;
static dma_addr_t __xdma_sink_dma_address;
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

static int KV[XDMA_SLOTS];

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

struct xdma_work {

	int nid;
	struct work_hdr header;
	struct xdma_work *next;
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

struct xdma_poll_wb {
	u32 completed_desc_count;
	u32 reserved_1[7];
} __packed;


const unsigned int rb_alloc_header_magic = 0xbad7face;

static DEFINE_SPINLOCK(send_work_pool_lock);
static DEFINE_SPINLOCK(send_queue_lock);
static DEFINE_SPINLOCK(xdma_lock);
static DEFINE_SPINLOCK(__xdma_slots_lock);
static DEFINE_SPINLOCK(xdma_work_pool_lock);

static struct ring_buffer xdma_send_buff = {};
static struct send_work *send_work_pool = NULL;

static queue_t *send_queue;
static queue_tr *recv_queue;

static struct xdma_work *xdma_work_pool = NULL;

static void __update_recv_index(queue_tr *q, int i)
{
	dma_addr_t dma_addr;
	int ret;

	if (i == q->nr_entries)	{
		i = 0;
		q->tail = -1;
	}
	dma_addr = q->work_list[i]->dma_addr;
	ret = config_descriptors_bypass(dma_addr, FDSM_MSG_SIZE, FROM_DEVICE, KMSG);
}

/* Upon completion of a send */

static void __process_sent(struct send_work *work)
{
	if (work->done)
		complete(work->done);
}


/* Enqueue message in send queue - Multi-threaded support.
 * Not required for single-threaded or serial applications.
 */

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


/* Dequeue message in send queue - Multi-threaded support.
 * Not required for single-threaded or serial applications.
 */

static int deq_send(queue_t *q)
{
	struct send_work *work;
	struct pcn_kmsg_message *msg;
	int ret, i;
	
	do {

		ret = down_interruptible(&q_empty);
		if (ret == -EINTR || kthread_should_stop()){
			return 0;
		}
	} while (ret);

	spin_lock(&send_queue_lock);
	work = q->work_list[q->head];
	q->head = (q->head + 1) % q->nr_entries;
	q->size--;
	spin_unlock(&send_queue_lock);
	up(&q_full);
	spin_lock(&xdma_lock);
	ret = config_descriptors_bypass(work->dma_addr, work->length, TO_DEVICE, KMSG);
	ret = xdma_transfer(TO_DEVICE);
	spin_unlock(&xdma_lock);
	__process_sent(work);
	return 0;

out:
	PCNPRINTK("Sending KMSG Failed!\n");
	return 1;
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

	if (msg->header.type < 0 || msg->header.type >= PCN_KMSG_TYPE_MAX) {
		pcn_kmsg_xdma_process(PCN_KMSG_TYPE_PROT_PROC_REQUEST, recv_queue->work_list[recv_i]->addr);	
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

/* Global XDMA work pool - DEPRECATED */

static int __refill_xdma_work(int xdma_slot)
{
	int i;
	int nr_refilled = 0;

	struct xdma_work *work_list = NULL;
	struct xdma_work *last_work = NULL;
	for (i = 0; i < xdma_slot; i++) {
		struct xdma_work *xw;

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
	spin_lock(&xdma_work_pool_lock);

	if (work_list) {
		last_work->next = xdma_work_pool;
		xdma_work_pool = work_list;
	}

	spin_unlock(&xdma_work_pool_lock);
	BUG_ON(nr_refilled == 0);
	return nr_refilled;

}

static struct xdma_work *__get_xdma_work(dma_addr_t dma_addr, void *addr, size_t size, dma_addr_t raddr)
{
	struct xdma_work *xw;
	spin_lock(&xdma_work_pool_lock);
	xw = xdma_work_pool;
	xdma_work_pool = xdma_work_pool->next;
	spin_unlock(&xdma_work_pool_lock);
	if (!xdma_work_pool) {

		__refill_xdma_work(XDMA_SLOTS);
	}

	xw->dma_addr = dma_addr;
	xw->addr = addr;
	xw->length = size;
	xw->remote_addr = raddr;
	return xw;
}

static void __put_xdma_work(struct xdma_work *xw)
{
	spin_lock(&xdma_work_pool_lock);
	xw->next = xdma_work_pool;
	xdma_work_pool = xw;
	spin_unlock(&xdma_work_pool_lock);
}

static void __put_xdma_send_work(struct send_work *work)
{
	unsigned long flags;
	if (test_bit(SW_FLAG_MAPPED, &work->flags)) {
		dma_unmap_single(&pci_dev->dev,work->dma_addr, work->length, DMA_TO_DEVICE);
	}

	if (test_bit(SW_FLAG_FROM_BUFFER, &work->flags))	{
		if (unlikely(test_bit(SW_FLAG_MAPPED, &work->flags))) {
			kfree(work->addr);
		} else {
			ring_buffer_put(&xdma_send_buff, work->addr);
		}
	}

	spin_lock_irqsave(&send_work_pool_lock, flags);
	work->next = send_work_pool;
	send_work_pool = work;
	spin_unlock_irqrestore(&send_work_pool_lock, flags);
}

/* To send kernel messages to the other node */

int xdma_kmsg_send(int nid, struct pcn_kmsg_message *msg, size_t size)
{
	struct send_work *work;
	int ret, i;
	DECLARE_COMPLETION_ONSTACK(done);

	work = __get_send_work(send_queue->tail);
	memcpy(work->addr, msg, size);
	work->done = &done;
	spin_lock(&xdma_lock);
	ret = config_descriptors_bypass(work->dma_addr, FDSM_MSG_SIZE, TO_DEVICE, KMSG);
	ret = xdma_transfer(TO_DEVICE);
	spin_unlock(&xdma_lock);

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
	__put_xdma_send_work(work);
	return ret;
}

/* Send messages to remote node */

int xdma_kmsg_post(int nid, struct pcn_kmsg_message *msg, size_t size)
{
	int ret;
	dma_addr_t dma_addr;
	dma_addr = radix_tree_lookup(&send_tree, (unsigned long *)msg);

	if (dma_addr) {
		spin_lock(&xdma_lock);
		ret = config_descriptors_bypass(dma_addr, FDSM_MSG_SIZE, TO_DEVICE, KMSG);
		ret = xdma_transfer(TO_DEVICE);
		spin_unlock(&xdma_lock);
	} else {
		printk("DMA addr: not found\n");
	}
	return 0;
}


/* To perform of DMA of pages requested by the remote node - DEPRECATED */

int xdma_kmsg_write(int to_nid, dma_addr_t raddr, void *addr, size_t size)
{
	//DECLARE_COMPLETION_ONSTACK(done);
	struct xdma_work *xw;
	dma_addr_t dma_addr;
	int ret;
	dma_addr = dma_map_single(&pci_dev->dev,addr, size, DMA_TO_DEVICE);
	ret = dma_mapping_error(&pci_dev->dev,dma_addr);

	if (!((u32)(dma_addr & XDMA_LSB_MASK))) {
		dma_addr = dma_map_single(&pci_dev->dev,addr, size, DMA_TO_DEVICE);
		ret = dma_mapping_error(&pci_dev->dev,dma_addr);
	}

	BUG_ON(ret);
	xw = __get_xdma_work(dma_addr, addr, size, raddr);
	BUG_ON(!xw);
	spin_lock(&xdma_lock);
	ret = config_descriptors_bypass(xw->dma_addr, size, TO_DEVICE, PAGE);
	ret = xdma_transfer(TO_DEVICE);
	spin_unlock(&xdma_lock);

out:
	dma_unmap_single(&pci_dev->dev,dma_addr, size, DMA_TO_DEVICE);
	__put_xdma_work(xw);
	return ret;
}

void xdma_kmsg_put(struct pcn_kmsg_message *msg)
{
	/* 
	struct rb_alloc_header *rbah = (struct rb_alloc_header *)msg - 1;
	struct send_work *work = rbah->work;
	__put_xdma_send_work(work); */
}

static int __config_pcie(struct pci_dev *dev)
{
	int ret;
	pci_dev_put(pci_dev);

	ret = pci_enable_device(pci_dev);
	if (ret) return ret;

	return 0;
}

static unsigned long __pci_map(struct pci_dev *dev, int BAR)
{
	unsigned long addr = pci_resource_start(pci_dev, BAR);
	if (!addr) {
		return 0;
	}

	return addr;
}

struct pcn_kmsg_message *xdma_kmsg_get(size_t size)
{
	struct send_work *work = __get_send_work(send_queue->tail);
	return (struct pcn_kmsg_message *)(work->addr);
}

int xdma_kmsg_read(int from_nid, void *addr, dma_addr_t raddr, size_t size)
{
	return -EPERM;
}

void xdma_kmsg_done(struct pcn_kmsg_message *msg)
{
}

void xdma_kmsg_stat(struct seq_file *seq, void *v)
{
	if (seq) {
		seq_printf(seq, POPCORN_STAT_FMT,
			   (unsigned long long)ring_buffer_usage(&xdma_send_buff),
#ifdef CONFIG_POPCORN_STAT
			   (unsigned long long)xdma_send_buff.peak_usage,
#else
			   0ULL,
#endif
			   "Send buffer usage");
	}
}

static int send_handler(void* arg0)
{
	int i;

	while (!kthread_should_stop())
	{
			i = deq_send(send_queue);
			if (i) {
				printk(KERN_ERR "Error sending message\n");
			}
		}

	return 0;
}

/* Polling KThread Handler */

static int poll_dma(void* arg0)
{
	bool was_frozen;

	struct xdma_poll_wb *poll_c2h_wb = (struct xdma_poll_wb *)c2h_poll_addr;
	struct xdma_poll_wb *poll_h2c_wb = (struct xdma_poll_wb *)h2c_poll_addr;
	u32 c2h_desc_complete = 0;
	u32 h2c_desc_complete = 0;
	int recv_index = 0, index = 0;

	while (!kthread_freezable_should_stop(&was_frozen))	{

		c2h_desc_complete = poll_c2h_wb->completed_desc_count;
		h2c_desc_complete = poll_h2c_wb->completed_desc_count;

		if (c2h_desc_complete != 0) {
			write_register(0x00, (u32 *)(xdma_c + c2h_ctl));
			write_register(0x06, (u32 *)(xdma_c + c2h_ch));
			index = __get_recv_index(recv_queue);
			__update_recv_index(recv_queue, index + 1);
			
			recv_index = recv_queue->size;
			poll_c2h_wb->completed_desc_count = 0;
			recv_queue->size += 1;
			if (recv_queue->size == recv_queue->nr_entries) {
				recv_queue->size = 0;
			}
			process_message(recv_index);
		} else if (h2c_desc_complete != 0) {
			no_of_messages += 1;
			write_register(0x00, (u32 *)(xdma_c + h2c_ctl));
			write_register(0x06, (u32 *)(xdma_c + h2c_ch));
			poll_h2c_wb->completed_desc_count = 0;
		}
	}

	return 0;
}

unsigned int queue_size(queue_t* q)
{
	if (q == NULL){
		return - 1;
	} else {
		return q->size;
	}
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
		send_q->work_list[i]->addr = base_addr + FDSM_MSG_SIZE * base_index;
		send_q->work_list[i]->dma_addr = base_dma + FDSM_MSG_SIZE * base_index;
		++base_index;
		radix_tree_insert(&send_tree, send_q->work_list[i]->addr, send_q->work_list[i]->dma_addr);
	}

	return send_q;

out:
	PCNPRINTK("Send Queue Failed\n");
	return NULL;
}

static void __update_xdma_index(dma_addr_t dma_addr, size_t size)
{
	config_descriptors_bypass(dma_addr, size, FROM_DEVICE, PAGE);
}

/* Buffer handling functions - DEPRECATED */ 

struct pcn_kmsg_xdma_handle *xdma_kmsg_pin_buffer(void *msg, size_t size)
{
	int ret;
	struct pcn_kmsg_xdma_handle *xh = kmalloc(sizeof(*xh), GFP_ATOMIC);
	spin_lock(&__xdma_slots_lock);
	
	xh->addr = __xdma_sink_address + XDMA_SLOT_SIZE * page_ix;
	xh->dma_addr =	__xdma_sink_dma_address + XDMA_SLOT_SIZE * page_ix;
	xh->flags = page_ix;
	KV[page_ix] = 1;
	__update_xdma_index(xh->dma_addr, PAGE_SIZE);
	page_ix += 1;
	spin_unlock(&__xdma_slots_lock);
	return xh;
}

void xdma_kmsg_unpin_buffer(struct pcn_kmsg_xdma_handle *handle)
{
	spin_lock(&__xdma_slots_lock);
	BUG_ON(!(KV[handle->flags]));
	KV[handle->flags] = 0;
	spin_unlock(&__xdma_slots_lock);
	kfree(handle);
}

struct pcn_kmsg_transport transport_xdma = {
	.name = "xdma",
	.features = PCN_KMSG_FEATURE_XDMA,

	.get = xdma_kmsg_get,
	.put = xdma_kmsg_put,
	.stat = xdma_kmsg_stat,

	.post = xdma_kmsg_post,
	.send = xdma_kmsg_send,
	.done = xdma_kmsg_done,

	.pin_xdma_buffer = xdma_kmsg_pin_buffer,
	.unpin_xdma_buffer = xdma_kmsg_unpin_buffer,
	.xdma_write = xdma_kmsg_write,
	.xdma_read = xdma_kmsg_read,

};


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

/* Ring Buffer Implementation - DEPRECATED */ 

static __init int __setup_ring_buffer(void)
{
	int ret;
	int i;

	/*Initialize send ring buffer */

	ret = ring_buffer_init(&xdma_send_buff, "dma_send");
	if (ret) return ret;

	for (i = 0; i < xdma_send_buff.nr_chunks; i++) {
		dma_addr_t dma_addr = dma_map_single(&pci_dev->dev,xdma_send_buff.chunk_start[i], RB_CHUNK_SIZE, DMA_TO_DEVICE);
		ret = dma_mapping_error(&pci_dev->dev,dma_addr);
		if (ret) goto out_unmap;
		xdma_send_buff.dma_addr_base[i] = dma_addr;
	}

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
	while (xdma_work_pool) {
		struct xdma_work *xw = xdma_work_pool;
		xdma_work_pool = xw->next;
		kfree(xw);
	}
	while (send_work_pool) {
		struct send_work *work = send_work_pool;
		send_work_pool = work->next;
		kfree(work);
	}
	for (i = 0; i < xdma_send_buff.nr_chunks; i++) {
		if (xdma_send_buff.dma_addr_base[i]) {
			dma_unmap_single(&pci_dev->dev,xdma_send_buff.dma_addr_base[i], RB_CHUNK_SIZE, DMA_TO_DEVICE);
			xdma_send_buff.dma_addr_base[i] = 0;
		}
	}
	return ret;

}

/* Interrupt Handler for monitoring the XDMA reads and writes */

static irqreturn_t xdma_isr(int irq, void *dev_id)
{
	unsigned long read_usr_irq, pkey, addr;
	int ret;

	total += 1;
	read_usr_irq = read_register(xdma_c + usr_irq);

	if (read_usr_irq & 0x01) {
		actual += 1;	
		user_interrupts_disable(KMSG);
		ret = xdma_transfer(FROM_DEVICE);
		user_interrupts_enable(KMSG);
		return IRQ_HANDLED;
	} else if (read_usr_irq & 0x02) {
		user_interrupts_disable(FAULT);
		printk(KERN_ERR "FAULT intr: %x\n", read_usr_irq);
		PCNPRINTK("PKEY: %lx and %lx\n", ioread32((u32 *)(xdma_x + wr_pkey_msb)), ioread32((u32 *)(xdma_x + wr_pkey_lsb)));
		user_interrupts_enable(FAULT);
		return IRQ_HANDLED;
	} else {
		/* Ignore this */
	}
	
	return IRQ_HANDLED;
}

/* Registering the IRQ Handler */

static int __setup_irq_handler(void)
{
	int ret;
	int irq = pci_dev->irq;

	ret = request_irq(irq, xdma_isr, 0, "PCN_XDMA", (void *)(xdma_isr));
	if (ret) return ret;

	return 0;
}

/* Polling thread handler initiation */

static int __start_poll(void)
{
	poll_tsk = kthread_run(poll_dma, NULL, "Poll_Handler");
	if (IS_ERR(poll_tsk)) {
		PCNPRINTK("Error Instantiating Polling Handler\n");
		return 1;
	}

	return 0;
}

static void __exit exit_kmsg_xdma(void)
{

	int i;

	/* Detach from messaging layer to avoid race conditions */

	pcn_kmsg_set_transport(NULL);
	set_popcorn_node_online(nid, false);

	//Unmap the physical address

	iounmap(xdma_c);
	iounmap(xdma_x);
	free_irq(pci_dev->irq, (void *)(xdma_isr));

	for (i = 0; i < xdma_send_buff.nr_chunks; i++)
	{
		if (xdma_send_buff.dma_addr_base[i]) {
			dma_unmap_single(&pci_dev->dev,xdma_send_buff.dma_addr_base[i], RB_CHUNK_SIZE, DMA_TO_DEVICE);
		}
	}

	while (send_work_pool) {
		struct send_work *work = send_work_pool;
		send_work_pool = work->next;
		kfree(work);
	}

	ring_buffer_destroy(&xdma_send_buff);
	
	if (domain)
		domain->ops->unmap(domain, base_dma, SZ_2M);
	
	if (send_queue)
		free_queue(send_queue);
	
	if (recv_queue)
		free_queue_r(recv_queue);

	while (xdma_work_pool) {
		struct xdma_work *xw = xdma_work_pool;
		xdma_work_pool = xw->next;
		kfree(xw);
	}

	destroy_workqueue(wq);

	dma_free_coherent(&pci_dev->dev, SZ_2M, base_addr, base_dma);
	dma_free_coherent(&pci_dev->dev, sizeof(struct xdma_poll_wb), c2h_poll_addr, c2h_poll_bus);
	dma_free_coherent(&pci_dev->dev, sizeof(struct xdma_poll_wb), h2c_poll_addr, h2c_poll_bus);

	if (tsk) {
		wake_up_process(tsk);
	}

	if (poll_tsk) {
		kthread_stop(poll_tsk);
	}

	PCNPRINTK("Popcorn message layer over XDMA unloaded\n");
	return;
}

static int __init init_kmsg_xdma(void)
{
	int i, ret, test_nid;
	int *test;
	unsigned long *pkey_msb, *pkey_lsb;
	unsigned long test_key; 
	struct dma_iommu_mapping *iommu_map;

	PCNPRINTK("\n ... Loading Popcorn messaging Layer over XDMA...\n");

	pcn_kmsg_set_transport(&transport_xdma);

	pci_dev = pci_get_device(VEND_ID, DEV_ID, NULL);
	if (pci_dev == NULL) goto out;

	ret = pci_set_dma_mask(pci_dev, DMA_BIT_MASK(32));
	dma_set_mask_and_coherent(&pci_dev->dev, DMA_BIT_MASK(32));

	ret =__config_pcie(pci_dev);
	if (ret){
		goto invalid;
	}


	ctl_address = __pci_map(pci_dev, CTL);
	if (!ctl_address) {
		PCNPRINTK("XDMA Configuration Failed\n");
		goto invalid;
	}

	axi_address = __pci_map(pci_dev, AXI);
	if (!axi_address) {
		PCNPRINTK("XDMA Configuration Failed\n");
		goto invalid;
	}

	xdma_c = ioremap(ctl_address, XDMA_SIZE);
	if (!xdma_c) goto invalid;

	xdma_x = ioremap(axi_address, AXI_SIZE);
	if (!xdma_x) goto invalid;

	ret = init_pcie_xdma(pci_dev, xdma_c, xdma_x);
	if (ret) {
		goto invalid;
	}
	
	PCNPRINTK("\n... XDMA Layer Configured ...\n");

	my_nid = 0;
	write_mynid(my_nid);
	set_popcorn_node_online(my_nid, true);
	pci_set_master(pci_dev);
	base_addr = dma_alloc_coherent(&pci_dev->dev, SZ_2M, &base_dma, GFP_KERNEL);

#ifdef CONFIG_ARM64 
		domain = iommu_get_domain_for_dev(&pci_dev->dev);
		if (!domain) goto out_free;
	
		ret = domain->ops->map(domain, base_dma, virt_to_phys(base_addr), SZ_2M, IOMMU_READ | IOMMU_WRITE);
#endif

	if (__setup_irq_handler())
		goto out_free;

	if (__setup_ring_buffer())
		goto out_free;

	wq = create_workqueue("recv");
	if (!wq)
		goto out_free;

	send_queue = __setup_send_queue(MAX_SEND_DEPTH);
	if (!send_queue) 
		goto out_free;

	recv_queue = __setup_recv_buffer(MAX_RECV_DEPTH);
	if (!recv_queue)
		goto out_free;

	memset(KV, 0, XDMA_SLOTS * sizeof(int));
	sema_init(&q_empty, 0);
	sema_init(&q_full, MAX_SEND_DEPTH);

	c2h_poll_addr = dma_alloc_coherent(&pci_dev->dev, sizeof(struct xdma_poll_wb), &c2h_poll_bus, GFP_KERNEL);
	h2c_poll_addr = dma_alloc_coherent(&pci_dev->dev, sizeof(struct xdma_poll_wb), &h2c_poll_bus, GFP_KERNEL);

	write_register(cpu_to_le32((c2h_poll_bus & XDMA_MSB_MASK) >> 32), xdma_c + c2h_poll_wr_msb);
	write_register(cpu_to_le32(c2h_poll_bus & XDMA_LSB_MASK), xdma_c + c2h_poll_wr_lsb);

	write_register(cpu_to_le32((h2c_poll_bus & XDMA_MSB_MASK) >> 32), xdma_c + h2c_poll_wr_msb);
	write_register(cpu_to_le32(h2c_poll_bus & XDMA_LSB_MASK), xdma_c + h2c_poll_wr_lsb);
	
#ifdef CONFIG_ARM64
		ret = domain->ops->map(domain, (unsigned long)h2c_poll_bus, virt_to_phys(h2c_poll_addr), PAGE_SIZE, IOMMU_READ | IOMMU_WRITE);
		if (ret) goto out_free;
			ret = domain->ops->map(domain, (unsigned long)c2h_poll_bus, virt_to_phys(c2h_poll_addr), PAGE_SIZE, IOMMU_READ | IOMMU_WRITE);
		if (ret) goto out_free;
#endif

	if (__start_poll()) 
		goto out_free;

	broadcast_my_node_info(2);
	PCNPRINTK("... Ready on XDMA ... \n");

	return 0;

out:
	PCNPRINTK("PCIe Device not found!!\n");
	exit_kmsg_xdma();
	return -EINVAL;

invalid:
	PCNPRINTK("DMA Bypass not found!..\n");
	exit_kmsg_xdma();
	return -EINVAL;

out_free:
	PCNPRINTK("Inside Out Free of INIT\n");
	exit_kmsg_xdma();
	return -EINVAL;

}

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Naarayanan");
MODULE_DESCRIPTION("XDMA Messaging Layer");

module_param(use_rb_thr, uint, 0644);
MODULE_PARM_DESC(use_rb_thr, "Threshold for using pre-allocated and pre-mapped ring buffer");

module_param_named(features, transport_xdma.features, ulong, 0644);
MODULE_PARM_DESC(use_xdma, "2: FPGA layer to transfer pages");

module_init(init_kmsg_xdma);
module_exit(exit_kmsg_xdma);


/* --- Deprecated Functions ---

static int __check_page_index(int i)
{
	if (i == XDMA_SLOTS) {
		if (KV[0] == 0) {
			page_ix = 0;
			return page_ix;
		} else {
			PCNPRINTK("Receive Buffer Full\n\r");
			while(KV[0] != 0);
			page_ix = 0;
			return 0;
		}
	} else if (KV[i]) {
		PCNPRINTK("Buffer not unpinned: %d and %d\n\r", KV[i], i);
		return -1;
	} else {
		return i;
	}
}


static __init int __setup_xdma_buffer(void)
{
	int ret, i;
	const int order = MAX_ORDER - 1;

	__xdma_sink_address = (void *)__get_free_pages(GFP_KERNEL, order);
	if (!__xdma_sink_address) return -EINVAL;

	__xdma_sink_dma_address = dma_map_single(&pci_dev->dev,__xdma_sink_address, 1 << (PAGE_SHIFT + order), DMA_FROM_DEVICE);
	ret = dma_mapping_error(&pci_dev->dev,__xdma_sink_dma_address);
	if (ret) goto out_free;
	return 0;

out_free:
	free_pages((unsigned long)__xdma_sink_address, order);
	__xdma_sink_address = NULL;
	return ret;
}


static void process_msg(struct work_struct *work)
{

	struct pcn_kmsg_message *msg;
	int i;
	struct pcn_work *rw = (struct pcn_work *)work;
	msg = rw->addr; 
	pcn_kmsg_process(msg);
	
	kfree((void *)work);

}

static void __page_sent(struct xdma_work *xw)
{
	if (xw->done){
		complete(xw->done);
	}
}

static int __process_received(struct recv_work *rws)
{
	struct pcn_kmsg_message *msg;
	struct pcn_work *work;
	bool ret;
	int i;
	msg = rws->addr;
	if (msg->header.type < 0 || msg->header.type >= PCN_KMSG_TYPE_MAX || 
	 msg->header.size < 0 || msg->header.size > PCN_KMSG_MAX_SIZE){
		printk(KERN_ERR "------- Faulty Work Rejected -----------!!\n");
		return 0;
	}
	
	work = kmalloc(sizeof(*work), GFP_ATOMIC);

	INIT_WORK((struct work_struct *)work, process_msg);
	work->addr = rws->addr;
	ret = queue_work(wq, (struct work_struct *)work);

	if (ret == false) {
		PCNPRINTK("Work already exists\n");
		return 1;
	}
	return 0;
}


static struct send_work *__get_xdma_send_work_map(struct pcn_kmsg_message *msg, size_t size)
{
	unsigned long flags;
	struct send_work *work;
	void *map_start = NULL;

	spin_lock_irqsave(&send_work_pool_lock, flags);
	work = send_work_pool;
	send_work_pool = work->next;
	spin_unlock_irqrestore(&send_work_pool_lock, flags);

	work->done = NULL;
	work->flags = 0;

	if (!msg) {
		struct rb_alloc_header *rbah;
		work->addr = ring_buffer_get_mapped(&xdma_send_buff, 
			sizeof(struct rb_alloc_header) + size, &work->dma_addr);

		if (likely(work->addr)) {
			work->dma_addr += sizeof(struct rb_alloc_header);
		} else {
			// Kmalloc when the ring buffer is full 
			work->addr = kmalloc(sizeof(struct rb_alloc_header) + size, GFP_ATOMIC);
			map_start = work->addr + sizeof(struct rb_alloc_header);
			set_bit(SW_FLAG_FROM_BUFFER, &work->flags);
		}

		rbah = work->addr;
		rbah->work = work;
	} else {
		work->addr = msg;
		map_start = work->addr;
	}

	if (map_start) {
		int ret;
		work->dma_addr = dma_map_single(&pci_dev->dev,map_start, size, DMA_TO_DEVICE);
		ret = dma_mapping_error(&pci_dev->dev,work->dma_addr);
		BUG_ON(ret);
		set_bit(SW_FLAG_MAPPED, &work->flags);

	}

	work->length = size;
	return work;
}

static struct send_work *__get_xdma_send_work(size_t size)
{
	return __get_xdma_send_work_map(NULL, size);
}


*/ 
