#include <linux/module.h>
#include <linux/bitmap.h>
#include <linux/seq_file.h>

#include <rdma/rdma_cm.h>
#include <popcorn/stat.h>

#include "common.h"
#include "ring_buffer.h"

#define RDMA_PORT 11453
#define RDMA_ADDR_RESOLVE_TIMEOUT_MS 5000

#define CONFIG_FILE_LEN 256
#define CONFIG_FILE_PATH        "/etc/popcorn/nodes_rdma"
#define CONFIG_FILE_CHUNK_SIZE  512

#define MAX_RECV_DEPTH	((PAGE_SIZE << (MAX_ORDER - 1)) / PCN_KMSG_MAX_SIZE)
#define MAX_SEND_DEPTH	(MAX_RECV_DEPTH)
#define RDMA_SLOT_SIZE	(PAGE_SIZE * 2)
#define NR_RDMA_SLOTS	((PAGE_SIZE << (MAX_ORDER - 1)) / RDMA_SLOT_SIZE)

static unsigned int use_rb_thr = PAGE_SIZE / 2;
static char config_file_path[CONFIG_FILE_LEN];

struct work_header {
	enum {
		WORK_TYPE_RECV,
		WORK_TYPE_SEND,
		WORK_TYPE_RDMA,
	} type;
};

struct recv_work {
	struct work_header header;
	struct ib_sge sgl;
	struct ib_recv_wr wr;
	dma_addr_t dma_addr;
	void *addr;
};

enum {
	SW_FLAG_MAPPED = 0,
	SW_FLAG_FROM_BUFFER = 1,
};

struct send_work {
	struct work_header header;
	struct send_work *next;
	struct ib_sge sgl;
	struct ib_send_wr wr;
	void *addr;
	unsigned long flags;
	struct completion *done;
};

struct rdma_work {
	struct work_header header;
	struct rdma_work *next;
	struct ib_sge sgl;
	struct ib_rdma_wr wr;
	struct completion *done;
};

struct rdma_handle {
	int nid;
	enum {
		RDMA_INIT,
		RDMA_ADDR_RESOLVED,
		RDMA_ROUTE_RESOLVED,
		RDMA_CONNECTING,
		RDMA_CONNECTED,
		RDMA_CLOSING,
		RDMA_CLOSED,
	} state;
	struct completion cm_done;

	struct recv_work *recv_works;
	void *recv_buffer;
	dma_addr_t recv_buffer_dma_addr;

	struct rdma_cm_id *cm_id;
	struct ib_device *device;
	struct ib_cq *cq;
	struct ib_qp *qp;
};

/* RDMA handle for each node */
static struct rdma_handle *rdma_handles[MAX_NUM_NODES] = { NULL };

/* Global protection domain (pd) and memory region (mr) */
static struct ib_pd *rdma_pd = NULL;
static struct ib_mr *rdma_mr = NULL;

/* Global RDMA sink */
static DEFINE_SPINLOCK(__rdma_slots_lock);
static DECLARE_BITMAP(__rdma_slots, NR_RDMA_SLOTS) = {0};
static char *__rdma_sink_addr;
static dma_addr_t __rdma_sink_dma_addr;


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
		bytes_read = kernel_read(fp, ip_addr, CONFIG_FILE_CHUNK_SIZE, &offset);
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
static inline int __get_rdma_buffer(void **addr, dma_addr_t *dma_addr) {
	int i;
	do {
		spin_lock(&__rdma_slots_lock);
		i = find_first_zero_bit(__rdma_slots, NR_RDMA_SLOTS);
		if (i < NR_RDMA_SLOTS) break;
		spin_unlock(&__rdma_slots_lock);
		WARN_ON_ONCE("recv buffer is full");
		io_schedule();
	} while (i >= NR_RDMA_SLOTS);
	set_bit(i, __rdma_slots);
	spin_unlock(&__rdma_slots_lock);

	if (addr) {
		*addr = __rdma_sink_addr + RDMA_SLOT_SIZE * i;
	}
	if (dma_addr) {
		*dma_addr = __rdma_sink_dma_addr + RDMA_SLOT_SIZE * i;
	}
	return i;
}

static inline void __put_rdma_buffer(int slot) {
	spin_lock(&__rdma_slots_lock);
#ifdef CONFIG_POPCORN_CHECK_SANITY
	BUG_ON(!test_bit(slot, __rdma_slots));
#endif
	clear_bit(slot, __rdma_slots);
	spin_unlock(&__rdma_slots_lock);
}


/* Global send buffer */
struct rb_alloc_header {
	struct send_work *sw;
	unsigned int flags;
#ifdef CONFIG_POPCORN_CHECK_SANITY
	unsigned int magic;
#endif
};
const unsigned int rb_alloc_header_magic = 0xbad7face;

static DEFINE_SPINLOCK(send_work_pool_lock);
static struct ring_buffer send_buffer = {};
static struct send_work *send_work_pool = NULL;

static struct send_work *__get_send_work_map(struct pcn_kmsg_message *msg, size_t size)
{
	unsigned long flags;
	struct send_work *sw;
	void *map_start = NULL;

	spin_lock_irqsave(&send_work_pool_lock, flags);
	BUG_ON(!send_work_pool);
	sw = send_work_pool;
	send_work_pool = sw->next;
	spin_unlock_irqrestore(&send_work_pool_lock, flags);

	sw->done = NULL;
	sw->flags = 0;

	if (!msg) {
		struct rb_alloc_header *ah;
		sw->addr = ring_buffer_get_mapped(&send_buffer,
				sizeof(struct rb_alloc_header) + size, &sw->sgl.addr);
		if (likely(sw->addr)) {
			sw->sgl.addr += sizeof(struct rb_alloc_header);
		} else {
			/* Fall back to kmalloc in case of buffer full */
#ifdef CONFIG_POPCORN_CHECK_SANITY
			if (WARN_ON_ONCE("ring buffer is full")) {
				printk(KERN_WARNING"ring buffer utilization: %lu\n",
					ring_buffer_usage(&send_buffer));
			}
#endif
			sw->addr = kmalloc(
					sizeof(struct rb_alloc_header) + size, GFP_ATOMIC);
			BUG_ON(!sw->addr);
			map_start = sw->addr + sizeof(struct rb_alloc_header);
		}
		ah = sw->addr;
		ah->sw = sw;
#ifdef CONFIG_POPCORN_CHECK_SANITY
		ah->magic = rb_alloc_header_magic;
#endif
		set_bit(SW_FLAG_FROM_BUFFER, &sw->flags);
	} else {
		sw->addr = msg;
		map_start = sw->addr;
	}

	if (map_start) {
		int ret;
		sw->sgl.addr = ib_dma_map_single(
				rdma_pd->device, map_start, size, DMA_TO_DEVICE);
		ret = ib_dma_mapping_error(rdma_pd->device, sw->sgl.addr);
		BUG_ON(ret);
		set_bit(SW_FLAG_MAPPED, &sw->flags);
	}
	sw->sgl.length = size;	/* Should be updated before sending out */
	return sw;
}

static struct send_work *__get_send_work(size_t size)
{
	return __get_send_work_map(NULL, size);
}

static void __put_send_work(struct send_work *sw)
{
	unsigned long flags;

	if (test_bit(SW_FLAG_MAPPED, &sw->flags)) {
		ib_dma_unmap_single(rdma_pd->device,
				sw->sgl.addr, sw->sgl.length, DMA_TO_DEVICE);
	}
	if (test_bit(SW_FLAG_FROM_BUFFER, &sw->flags)) {
#ifdef CONFIG_POPCORN_CHECK_SANITY
		BUG_ON(((struct rb_alloc_header *)sw->addr)->magic !=
				rb_alloc_header_magic);
#endif
		if (unlikely(test_bit(SW_FLAG_MAPPED, &sw->flags))) {
			kfree(sw->addr);
		} else {
			ring_buffer_put(&send_buffer, sw->addr);
		}
	}

	spin_lock_irqsave(&send_work_pool_lock, flags);
	sw->next = send_work_pool;
	send_work_pool = sw;
	spin_unlock_irqrestore(&send_work_pool_lock, flags);
}


/* Global RDMA work pool */
static DEFINE_SPINLOCK(rdma_work_pool_lock);
static struct rdma_work *rdma_work_pool = NULL;
static int __refill_rdma_work(int nr_works)
{
	int i;
	int nr_refilled = 0;
	struct rdma_work *work_list = NULL;
	struct rdma_work *last_work = NULL;
	for (i = 0; i < nr_works; i++) {
		struct rdma_work *rw;

		rw = kzalloc(sizeof(*rw), GFP_KERNEL);
		if (!rw) goto out;

		rw->header.type = WORK_TYPE_RDMA;

		rw->sgl.addr = 0;
		rw->sgl.length = 0;
		rw->sgl.lkey = rdma_pd->local_dma_lkey;

		rw->wr.wr.next = NULL;
		rw->wr.wr.wr_id = (u64)rw;
		rw->wr.wr.sg_list = &rw->sgl;
		rw->wr.wr.num_sge = 1;
		rw->wr.wr.opcode = IB_WR_RDMA_WRITE; // IB_WR_RDMA_WRITE_WITH_IMM;
		rw->wr.wr.send_flags = IB_SEND_SIGNALED;
		rw->wr.remote_addr = 0;
		rw->wr.rkey = 0;

		if (!last_work) last_work = rw;
		rw->next = work_list;
		work_list = rw;
		nr_refilled++;
	}

out:
	spin_lock(&rdma_work_pool_lock);
	if (work_list) {
		last_work->next = rdma_work_pool;
		rdma_work_pool = work_list;
	}
	spin_unlock(&rdma_work_pool_lock);
	BUG_ON(nr_refilled == 0);
	return nr_refilled;
}

static struct rdma_work *__get_rdma_work(dma_addr_t dma_addr, size_t size, dma_addr_t rdma_addr, u32 rdma_key)
{
	struct rdma_work *rw;
	might_sleep();

	spin_lock(&rdma_work_pool_lock);
	rw = rdma_work_pool;
	rdma_work_pool = rdma_work_pool->next;
	spin_unlock(&rdma_work_pool_lock);

	if (!rdma_work_pool) {
		__refill_rdma_work(NR_RDMA_SLOTS);
	}

	rw->sgl.addr = dma_addr;
	rw->sgl.length = size;

	rw->wr.remote_addr = rdma_addr;
	rw->wr.rkey = rdma_key;
	return rw;
}

static void __put_rdma_work(struct rdma_work *rw)
{
	might_sleep();
	spin_lock(&rdma_work_pool_lock);
	rw->next = rdma_work_pool;
	rdma_work_pool = rw;
	spin_unlock(&rdma_work_pool_lock);
}


/* Buffer management */
struct pcn_kmsg_message *rdma_kmsg_get(size_t size)
{
	struct send_work *sw = __get_send_work(size);
	struct rb_alloc_header *ah = sw->addr;
#ifdef CONFIG_POPCORN_CHECK_SANITY
	BUG_ON(!test_bit(SW_FLAG_FROM_BUFFER, &sw->flags));
	BUG_ON(ah->magic != rb_alloc_header_magic);
#endif
	return (struct pcn_kmsg_message *)(ah + 1);
}

void rdma_kmsg_put(struct pcn_kmsg_message *msg)
{
	struct rb_alloc_header *ah = (struct rb_alloc_header *)msg - 1;
	struct send_work *sw = ah->sw;
	__put_send_work(sw);
}

void rdma_kmsg_stat(struct seq_file *seq, void *v)
{
	if (seq) {
		seq_printf(seq, POPCORN_STAT_FMT,
				(unsigned long long)ring_buffer_usage(&send_buffer),
#ifdef CONFIG_POPCORN_STAT
				(unsigned long long)send_buffer.peak_usage,
#else
				0ULL,
#endif
				"Send buffer usage");
	}
}


/****************************************************************************
 * Send
 */
static int __send_to(int to_nid, struct send_work *sw, size_t size)
{
	struct rdma_handle *rh = rdma_handles[to_nid];
	const struct ib_send_wr *bad_wr = NULL;
	int ret;

#ifdef CONFIG_POPCORN_CHECK_SANITY
	BUG_ON(size > sw->sgl.length);
#endif
	sw->sgl.length = size; /* Might be shrunk after get*/

	ret = ib_post_send(rh->qp, &sw->wr, &bad_wr);
	if (ret) return ret;
	if (bad_wr) return -EINVAL;

	return 0;
}

int rdma_kmsg_send(int dst, struct pcn_kmsg_message *msg, size_t size)
{
	struct send_work *sw;
	DECLARE_COMPLETION_ONSTACK(done);
	int ret;
	might_sleep();

	if (size <= use_rb_thr) {
		sw = __get_send_work(size);
		memcpy(sw->addr + sizeof(struct rb_alloc_header), msg, size);
	} else {
		sw = __get_send_work_map(msg, size);
	}

	sw->done = &done;

	ret = __send_to(dst, sw, size);
	if (ret) goto out;

	if (!try_wait_for_completion(&done)) {
		ret = wait_for_completion_io_timeout(&done, 60 * HZ);
		if (!ret) {
			ret = -ETIME;
			goto out;
		}
	}
	/* send_work is returned in the completion handler */
	return 0;
out:
	__put_send_work(sw);
	return ret;
}

int rdma_kmsg_post(int dst, struct pcn_kmsg_message *msg, size_t size)
{
	struct rb_alloc_header *ah = (struct rb_alloc_header *)msg - 1;
	struct send_work *sw = ah->sw;
	int ret;

#ifdef CONFIG_POPCORN_CHECK_SANITY
	if (!test_bit(SW_FLAG_MAPPED, &sw->flags)) {
		BUG_ON(ah->magic != rb_alloc_header_magic&& "compromised send buffer");
	}
#endif

	ret = __send_to(dst, sw, size);
	if (ret) {
		__put_send_work(sw);
		return ret;
	}
	/* send_work is returned in the completion handler */
	return 0;
}


/****************************************************************************
 * Perform RDMA
 */
struct pcn_kmsg_rdma_handle *rdma_kmsg_pin_rdma_buffer(void *msg, size_t size)
{
	struct pcn_kmsg_rdma_handle *rh = kmalloc(sizeof(*rh), GFP_KERNEL);

	if (!rh) return ERR_PTR(-ENOMEM);

#ifdef CONFIG_POPCORN_CHECK_SANITY
	if (size > RDMA_SLOT_SIZE) {
		BUG_ON("Too large buffer to pin");
		return ERR_PTR(-EINVAL);
	}
#endif
	rh->rkey = rdma_mr->rkey;
	rh->private = (void *)
		(unsigned long)__get_rdma_buffer(&rh->addr, &rh->dma_addr);

	return rh;
}

void rdma_kmsg_unpin_rdma_buffer(struct pcn_kmsg_rdma_handle *handle)
{
	__put_rdma_buffer((unsigned long)handle->private);
	kfree(handle);
}

int rdma_kmsg_write(int to_nid, dma_addr_t rdma_addr, void *addr, size_t size, u32 rdma_key)
{
	DECLARE_COMPLETION_ONSTACK(done);
	struct rdma_work *rw;
	const struct ib_send_wr *bad_wr = NULL;

	dma_addr_t dma_addr;
	int ret;

	dma_addr = ib_dma_map_single(rdma_mr->device, addr, size, DMA_TO_DEVICE);
	ret = ib_dma_mapping_error(rdma_mr->device, dma_addr);
	BUG_ON(ret);

	rw = __get_rdma_work(dma_addr, size, rdma_addr, rdma_key);
	BUG_ON(!rw);

	rw->done = &done;

	ret = ib_post_send(rdma_handles[to_nid]->qp, &rw->wr.wr, &bad_wr);
	if (ret || bad_wr) {
		printk("Cannot post rdma write, %d, %p\n", ret, bad_wr);
		if (ret == 0) ret = -EINVAL;
		goto out;
	}
	/* XXX polling??? */
	if (!try_wait_for_completion(&done)) {
		wait_for_completion(&done);
	}

out:
	ib_dma_unmap_single(rdma_mr->device, dma_addr, size, DMA_TO_DEVICE);
	__put_rdma_work(rw);
	return ret;
}

int rdma_kmsg_read(int from_nid, void *addr, dma_addr_t rdma_addr, size_t size, u32 rdma_key)
{
	return -EPERM;
}


/****************************************************************************
 * Event handlers
 */
void rdma_kmsg_done(struct pcn_kmsg_message *msg)
{
	/* Put back the receive work */
	int ret;
	const struct ib_recv_wr *bad_wr = NULL;
	int from_nid = PCN_KMSG_FROM_NID(msg);
	struct rdma_handle *rh = rdma_handles[from_nid];
	int index = ((void *)msg - rh->recv_buffer) / PCN_KMSG_MAX_SIZE;

#ifdef CONFIG_POPCORN_CHECK_SANITY
	BUG_ON(index < 0 || index >= MAX_RECV_DEPTH);
#endif

	ret = ib_post_recv(rh->qp, &rh->recv_works[index].wr, &bad_wr);
	BUG_ON(ret || bad_wr);
}

static void __process_recv(struct ib_wc *wc)
{
	struct recv_work *rw = (void *)wc->wr_id;
	/*
	printk("recv %d %d\n", wc->byte_len,
			((struct pcn_kmsg_message *)rw->addr)->header.type);
	*/
	pcn_kmsg_process(rw->addr);
}

static void __process_sent(struct ib_wc *wc)
{
	struct send_work *sw = (void *)wc->wr_id;

	if (sw->done) {
		complete(sw->done);
	}
	__put_send_work(sw);
}

static void __process_rdma_completion(struct ib_wc *wc)
{
	struct rdma_work *rw = (void *)wc->wr_id;
	complete(rw->done);
}

static void __process_comp_wakeup(struct ib_wc *wc, const char *msg)
{
	struct completion *done = (void *)wc->wr_id;
	complete(done);
}

static void __process_faulty_work(struct ib_wc *wc)
{
	struct work_header *header = (struct work_header *)wc->wr_id;

	printk("abnormal status %d with %d, %p\n", wc->status, wc->opcode, header);
	switch(header->type) {
	case WORK_TYPE_SEND: {
		struct send_work *w = (struct send_work *)wc->wr_id;
		struct pcn_kmsg_message *msg;
		printk("  type: send, %llx + %d\n", w->sgl.addr, w->sgl.length);
		if (test_bit(SW_FLAG_FROM_BUFFER, &w->flags)) {
			msg = w->addr + sizeof(struct rb_alloc_header);
		} else {
			msg = w->addr;
		}
		printk("  message: %d %d %ld\n",
				msg->header.from_nid, msg->header.type, msg->header.size);
		break;
	}
	case WORK_TYPE_RECV: {
		struct recv_work *w = (struct recv_work *)wc->wr_id;
		printk("  type: recv, %llx + %d\n", w->sgl.addr, w->sgl.length);
		break;
	}
	case WORK_TYPE_RDMA: {
		struct rdma_work *w = (struct rdma_work *)wc->wr_id;
		printk("  type: rdma, %llx + %d\n", w->sgl.addr, w->sgl.length);
		break;
	}
	default:
		printk("  Unknown type\n");
		break;
	}
}

void cq_comp_handler(struct ib_cq *cq, void *context)
{
	int ret;
	struct ib_wc wc;

retry:
	while ((ret = ib_poll_cq(cq, 1, &wc)) > 0) {
		if (wc.opcode < 0 || wc.status) {
			__process_faulty_work(&wc);
			continue;
		}
		switch(wc.opcode) {
		case IB_WC_SEND:
			__process_sent(&wc);
			break;
		case IB_WC_RECV:
			__process_recv(&wc);
			break;
		case IB_WC_RDMA_WRITE:
		case IB_WC_RDMA_READ:
			__process_rdma_completion(&wc);
			break;
		case IB_WC_REG_MR:
			__process_comp_wakeup(&wc, "mr registered\n");
			break;
		default:
			printk("Unknown completion op %d\n", wc.opcode);
			break;
		}
	}
	ret = ib_req_notify_cq(cq, IB_CQ_NEXT_COMP | IB_CQ_REPORT_MISSED_EVENTS);
	if (ret > 0) goto retry;
}


/****************************************************************************
 * Setup connections
 */
static __init int __setup_pd_cq_qp(struct rdma_handle *rh)
{
	int ret;

	BUG_ON(rh->state != RDMA_ROUTE_RESOLVED && "for rh->device");

	/* Create global pd if it is not allocated yet */
	if (!rdma_pd) {
		rdma_pd = ib_alloc_pd(rh->device,0);
		if (IS_ERR(rdma_pd)) {
			ret = PTR_ERR(rdma_pd);
			rdma_pd = NULL;
			goto out_err;
		}
	}

	/* create completion queue */
	if (!rh->cq) {
		struct ib_cq_init_attr cq_attr = {
			.cqe = MAX_SEND_DEPTH + MAX_RECV_DEPTH + NR_RDMA_SLOTS,
			.comp_vector = 0,
		};

		rh->cq = ib_create_cq(
				rh->device, cq_comp_handler, NULL, rh, &cq_attr);
		if (IS_ERR(rh->cq)) {
			ret = PTR_ERR(rh->cq);
			goto out_err;
		}

		ret = ib_req_notify_cq(rh->cq, IB_CQ_NEXT_COMP);
		if (ret < 0) goto out_err;
	}

	/* create queue pair */
	{
		struct ib_qp_init_attr qp_attr = {
			.event_handler = NULL, // qp_event_handler,
			.qp_context = rh,
			.cap = {
				.max_send_wr = MAX_SEND_DEPTH,
				.max_recv_wr = MAX_RECV_DEPTH + NR_RDMA_SLOTS,
				.max_send_sge = PCN_KMSG_MAX_SIZE >> PAGE_SHIFT,
				.max_recv_sge = PCN_KMSG_MAX_SIZE >> PAGE_SHIFT,
			},
			.sq_sig_type = IB_SIGNAL_REQ_WR,
			.qp_type = IB_QPT_RC,
			.send_cq = rh->cq,
			.recv_cq = rh->cq,
		};

		ret = rdma_create_qp(rh->cm_id, rdma_pd, &qp_attr);
		if (ret) goto out_err;
		rh->qp = rh->cm_id->qp;
	}
	return 0;

out_err:
	return ret;
}

static __init int __setup_buffers_and_pools(struct rdma_handle *rh)
{
	int ret = 0, i;
	dma_addr_t dma_addr;
	char *recv_buffer = NULL;
	struct recv_work *rws = NULL;
	const size_t buffer_size = PCN_KMSG_MAX_SIZE * MAX_RECV_DEPTH;

	/* Initalize receive buffers */
	recv_buffer = kmalloc(buffer_size, GFP_KERNEL);
	if (!recv_buffer) {
		return -ENOMEM;
	}
	rws = kmalloc(sizeof(*rws) * MAX_RECV_DEPTH, GFP_KERNEL);
	if (!rws) {
		ret = -ENOMEM;
		goto out_free;
	}

	/* Populate receive buffer and work requests */
	dma_addr = ib_dma_map_single(
			rh->device, recv_buffer, buffer_size, DMA_FROM_DEVICE);
	ret = ib_dma_mapping_error(rh->device, dma_addr);
	if (ret) goto out_free;

	for (i = 0; i < MAX_RECV_DEPTH; i++) {
		struct recv_work *rw = rws + i;
		struct ib_recv_wr *wr;
	        const struct ib_recv_wr *bad_wr = NULL;
		
		struct ib_sge *sgl;

		rw->header.type = WORK_TYPE_RECV;
		rw->dma_addr = dma_addr + PCN_KMSG_MAX_SIZE * i;
		rw->addr = recv_buffer + PCN_KMSG_MAX_SIZE * i;

		sgl = &rw->sgl;
		sgl->lkey = rdma_pd->local_dma_lkey;
		sgl->addr = rw->dma_addr;
		sgl->length = PCN_KMSG_MAX_SIZE;

		wr = &rw->wr;
		wr->sg_list = sgl;
		wr->num_sge = 1;
		wr->next = NULL;
		wr->wr_id = (u64)rw;

		ret = ib_post_recv(rh->qp, wr, &bad_wr);
		if (ret || bad_wr) goto out_free;
	}
	rh->recv_works = rws;
	rh->recv_buffer = recv_buffer;
	rh->recv_buffer_dma_addr = dma_addr;

	return ret;

out_free:
	if (recv_buffer) kfree(recv_buffer);
	if (rws) kfree(rws);
	return ret;
}

static __init int __setup_rdma_buffer(const int nr_chunks)
{
	int ret;
	DECLARE_COMPLETION_ONSTACK(done);
	struct ib_mr *mr = NULL;
	const struct ib_send_wr *bad_wr = NULL;
	struct ib_reg_wr reg_wr = {
		.wr = {
			.opcode = IB_WR_REG_MR,
			.send_flags = IB_SEND_SIGNALED,
			.wr_id = (u64)&done,
		},
		.access = IB_ACCESS_REMOTE_WRITE,
				  /*
				  IB_ACCESS_LOCAL_WRITE |
				  IB_ACCESS_REMOTE_READ |
				  */
	};
	struct scatterlist sg = {};
	const int alloc_order = MAX_ORDER - 1;

	__rdma_sink_addr = (void *)__get_free_pages(GFP_KERNEL, alloc_order);
	if (!__rdma_sink_addr) return -EINVAL;

	__rdma_sink_dma_addr = ib_dma_map_single(
			rdma_pd->device, __rdma_sink_addr, 1 << (PAGE_SHIFT + alloc_order),
			DMA_FROM_DEVICE);
	ret = ib_dma_mapping_error(rdma_pd->device, __rdma_sink_dma_addr);
	if (ret) goto out_free;

	mr = ib_alloc_mr(rdma_pd, IB_MR_TYPE_MEM_REG, 1 << alloc_order);
	if (IS_ERR(mr)) goto out_free;

	sg_dma_address(&sg) = __rdma_sink_dma_addr;
	sg_dma_len(&sg) = 1 << (PAGE_SHIFT + alloc_order);

	ret = ib_map_mr_sg(mr, &sg, 1,NULL, PAGE_SIZE);
	if (ret != 1) {
		printk("Cannot map scatterlist to mr, %d\n", ret);
		goto out_dereg;
	}
	reg_wr.mr = mr;
	reg_wr.key = mr->rkey;

	/**
	 * rdma_handles[my_nid] is for accepting connection without qp & cp.
	 * So, let's use rdma_handles[1] for nid 0 and rdma_handles[0] otherwise.
	 */
	ret = ib_post_send(rdma_handles[!my_nid]->qp, &reg_wr.wr,&bad_wr);
	if (ret || bad_wr) {
		printk("Cannot register mr, %d %p\n", ret, bad_wr);
		if (bad_wr) ret = -EINVAL;
		goto out_dereg;
	}
	ret = wait_for_completion_io_timeout(&done, 5 * HZ);
	if (!ret) {
		printk("Timed-out to register mr\n");
		ret = -ETIMEDOUT;
		goto out_dereg;
	}

	rdma_mr = mr;
	//printk("lkey: %x, rkey: %x, length: %x\n", mr->lkey, mr->rkey, mr->length);
	return 0;

out_dereg:
	ib_dereg_mr(mr);
	return ret;

out_free:
	free_pages((unsigned long)__rdma_sink_addr, alloc_order);
	__rdma_sink_addr = NULL;
	return ret;
}

static int __init __setup_work_request_pools(void)
{
	int ret;
	int i;

	/* Initialize send buffer */
	ret = ring_buffer_init(&send_buffer, "rdma_send");
	if (ret) return ret;

	for (i = 0; i < send_buffer.nr_chunks; i++) {
		dma_addr_t dma_addr = ib_dma_map_single(rdma_pd->device,
				send_buffer.chunk_start[i], RB_CHUNK_SIZE, DMA_TO_DEVICE);
		ret = ib_dma_mapping_error(rdma_pd->device, dma_addr);
		if (ret) goto out_unmap;
		send_buffer.dma_addr_base[i] = dma_addr;
	}

	/* Initialize send work request pool */
	for (i = 0; i < MAX_SEND_DEPTH; i++) {
		struct send_work *sw;

		sw = kzalloc(sizeof(*sw), GFP_KERNEL);
		if (!sw) {
			ret = -ENOMEM;
			goto out_unmap;
		}
		sw->header.type = WORK_TYPE_SEND;

		sw->sgl.addr = 0;
		sw->sgl.length = 0;
		sw->sgl.lkey = rdma_pd->local_dma_lkey;

		sw->wr.next = NULL;
		sw->wr.wr_id = (u64)sw;
		sw->wr.sg_list = &sw->sgl;
		sw->wr.num_sge = 1;
		sw->wr.opcode = IB_WR_SEND;
		sw->wr.send_flags = IB_SEND_SIGNALED;

		sw->next = send_work_pool;
		send_work_pool = sw;
	}

	/* Initalize rdma work request pool */
	__refill_rdma_work(NR_RDMA_SLOTS);
	return 0;

out_unmap:
	while (rdma_work_pool) {
		struct rdma_work *rw = rdma_work_pool;
		rdma_work_pool = rw->next;
		kfree(rw);
	}
	while (send_work_pool) {
		struct send_work *sw = send_work_pool;
		send_work_pool = sw->next;
		kfree(sw);
	}
	for (i = 0; i < send_buffer.nr_chunks; i++) {
		if (send_buffer.dma_addr_base[i]) {
			ib_dma_unmap_single(rdma_pd->device,
					send_buffer.dma_addr_base[i], RB_CHUNK_SIZE, DMA_TO_DEVICE);
			send_buffer.dma_addr_base[i] = 0;
		}
	}
	return ret;
}


/****************************************************************************
 * Client-side connection handling
 */
int cm_client_event_handler(struct rdma_cm_id *cm_id, struct rdma_cm_event *cm_event)
{
	struct rdma_handle *rh = cm_id->context;

	switch (cm_event->event) {
	case RDMA_CM_EVENT_ADDR_RESOLVED:
		rh->state = RDMA_ADDR_RESOLVED;
		complete(&rh->cm_done);
		break;
	case RDMA_CM_EVENT_ROUTE_RESOLVED:
		rh->state = RDMA_ROUTE_RESOLVED;
		complete(&rh->cm_done);
		break;
	case RDMA_CM_EVENT_ESTABLISHED:
		rh->state = RDMA_CONNECTED;
		complete(&rh->cm_done);
		break;
	case RDMA_CM_EVENT_DISCONNECTED:
		MSGPRINTK("Disconnected from %d\n", rh->nid);
		/* TODO deallocate associated resources */
		break;
	case RDMA_CM_EVENT_REJECTED:
	case RDMA_CM_EVENT_CONNECT_ERROR:
		complete(&rh->cm_done);
		break;
	case RDMA_CM_EVENT_ADDR_ERROR:
	case RDMA_CM_EVENT_ROUTE_ERROR:
	case RDMA_CM_EVENT_UNREACHABLE:
	default:
		printk("Unhandled client event %d\n", cm_event->event);
		break;
	}
	return 0;
}

static int __connect_to_server(int nid)
{
	int ret;
	const char *step;
	struct rdma_handle *rh = rdma_handles[nid];

	step = "create rdma id";
	rh->cm_id = rdma_create_id(&init_net,
			cm_client_event_handler, rh, RDMA_PS_IB, IB_QPT_RC);
	if (IS_ERR(rh->cm_id)) goto out_err;

	step = "resolve server address";
	{
		struct sockaddr_in addr = {
			.sin_family = AF_INET,
			.sin_port = htons(RDMA_PORT),
			.sin_addr.s_addr = ip_table[nid],
		};

		ret = rdma_resolve_addr(rh->cm_id, NULL,
				(struct sockaddr *)&addr, RDMA_ADDR_RESOLVE_TIMEOUT_MS);
		if (ret) goto out_err;
		ret = wait_for_completion_interruptible(&rh->cm_done);
		if (ret || rh->state != RDMA_ADDR_RESOLVED) goto out_err;
	}

	step = "resolve routing path";
	ret = rdma_resolve_route(rh->cm_id, RDMA_ADDR_RESOLVE_TIMEOUT_MS);
	if (ret) goto out_err;
	ret = wait_for_completion_interruptible(&rh->cm_done);
	if (ret || rh->state != RDMA_ROUTE_RESOLVED) goto out_err;

	/* cm_id->device is valid after the address and route are resolved */
	rh->device = rh->cm_id->device;

	step = "setup ib";
	ret = __setup_pd_cq_qp(rh);
	if (ret) goto out_err;

	step = "setup buffers and pools";
	ret = __setup_buffers_and_pools(rh);
	if (ret) goto out_err;

	step = "connect";
	{
		struct rdma_conn_param conn_param = {
			.private_data = &my_nid,
			.private_data_len = sizeof(my_nid),
		};

		rh->state = RDMA_CONNECTING;
		ret = rdma_connect(rh->cm_id, &conn_param);
		if (ret) goto out_err;
		ret = wait_for_completion_interruptible(&rh->cm_done);
		if (ret) goto out_err;
		if (rh->state != RDMA_CONNECTED) {
			ret = -ETIMEDOUT;
			goto out_err;
		}
	}

	MSGPRINTK("Connected to %d\n", nid);
	return 0;

out_err:
	PCNPRINTK_ERR("Unable to %s, %pI4, %d\n", step, ip_table + nid, ret);
	return ret;
}


/****************************************************************************
 * Server-side connection handling
 */
static int __accept_client(int nid)
{
	struct rdma_handle *rh = rdma_handles[nid];
	struct rdma_conn_param conn_param = {};
	int ret;

	ret = wait_for_completion_io_timeout(&rh->cm_done, 60 * HZ);
	if (!ret) return -ETIMEDOUT;
	if (rh->state != RDMA_ROUTE_RESOLVED) return -EINVAL;

	ret = __setup_pd_cq_qp(rh);
	if (ret) return ret;

	ret = __setup_buffers_and_pools(rh);
	if (ret) return ret;

	rh->state = RDMA_CONNECTING;
	ret = rdma_accept(rh->cm_id, &conn_param);
	if (ret) return ret;

	ret = wait_for_completion_interruptible(&rh->cm_done);
	if (ret) return ret;

	return 0;
}
static int __on_client_connecting(struct rdma_cm_id *cm_id, struct rdma_cm_event *cm_event)
{
	int peer_nid = *(int *)cm_event->param.conn.private_data;
	struct rdma_handle *rh = rdma_handles[peer_nid];

	cm_id->context = rh;
	rh->cm_id = cm_id;
	rh->device = cm_id->device;
	rh->state = RDMA_ROUTE_RESOLVED;

	complete(&rh->cm_done);
	return 0;
}

static int __on_client_connected(struct rdma_cm_id *cm_id, struct rdma_cm_event *cm_event)
{
	struct rdma_handle *rh = cm_id->context;
	rh->state = RDMA_CONNECTED;
	complete(&rh->cm_done);

	MSGPRINTK("Connected to %d\n", rh->nid);
	return 0;
}

static int __on_client_disconnected(struct rdma_cm_id *cm_id, struct rdma_cm_event *cm_event)
{
	struct rdma_handle *rh = cm_id->context;
	rh->state = RDMA_INIT;
	set_popcorn_node_online(rh->nid, false);

	MSGPRINTK("Disconnected from %d\n", rh->nid);
	return 0;
}

int cm_server_event_handler(struct rdma_cm_id *cm_id, struct rdma_cm_event *cm_event)
{
	int ret = 0;
	switch (cm_event->event) {
	case RDMA_CM_EVENT_CONNECT_REQUEST:
		ret = __on_client_connecting(cm_id, cm_event);
		break;
	case RDMA_CM_EVENT_ESTABLISHED:
		ret = __on_client_connected(cm_id, cm_event);
		break;
	case RDMA_CM_EVENT_DISCONNECTED:
		ret = __on_client_disconnected(cm_id, cm_event);
		break;
	default:
		MSGPRINTK("Unhandled server event %d\n", cm_event->event);
		break;
	}
	return 0;
}

static int __listen_to_connection(void)
{
	int ret;
	struct sockaddr_in addr = {
		.sin_family = AF_INET,
		.sin_port = htons(RDMA_PORT),
		.sin_addr.s_addr = ip_table[my_nid],
	};

	struct rdma_cm_id *cm_id = rdma_create_id(&init_net,
			cm_server_event_handler, NULL, RDMA_PS_IB, IB_QPT_RC);
	if (IS_ERR(cm_id)) return PTR_ERR(cm_id);
	rdma_handles[my_nid]->cm_id = cm_id;

	ret = rdma_bind_addr(cm_id, (struct sockaddr *)&addr);
	if (ret) {
		PCNPRINTK_ERR("Cannot bind server address, %d\n", ret);
		return ret;
	}

	ret = rdma_listen(cm_id, MAX_NUM_NODES);
	if (ret) {
		PCNPRINTK_ERR("Cannot listen to incoming requests, %d\n", ret);
		return ret;
	}

	return 0;
}


static int __establish_connections(void)
{
	int i, ret;

	ret = __listen_to_connection();
	if (ret) return ret;

	/* Wait for a while so that nodes are ready to listen to connections */
	msleep(100);

	for (i = 0; i < my_nid; i++) {
		if ((ret = __connect_to_server(i))) return ret;
		set_popcorn_node_online(i, true);
	}

	set_popcorn_node_online(my_nid, true);

	for (i = my_nid + 1; i < MAX_NUM_NODES; i++) {
		if ((ret = __accept_client(i))) return ret;
		set_popcorn_node_online(i, true);
	}

	MSGPRINTK("Connections are established.\n");
	return 0;
}

void __exit exit_kmsg_rdma(void)
{
	int i;

	/* Detach from upper layer to prevent race condition during exit */
	pcn_kmsg_set_transport(NULL);

	for (i = 0; i < MAX_NUM_NODES; i++) {
		struct rdma_handle *rh = rdma_handles[i];
		set_popcorn_node_online(i, false);
		if (!rh) continue;

		if (rh->recv_buffer) {
			ib_dma_unmap_single(rh->device, rh->recv_buffer_dma_addr,
					PCN_KMSG_MAX_SIZE * MAX_RECV_DEPTH, DMA_FROM_DEVICE);
			kfree(rh->recv_buffer);
			kfree(rh->recv_works);
		}

		if (rh->qp && !IS_ERR(rh->qp)) rdma_destroy_qp(rh->cm_id);
		if (rh->cq && !IS_ERR(rh->cq)) ib_destroy_cq(rh->cq);
		if (rh->cm_id && !IS_ERR(rh->cm_id)) rdma_destroy_id(rh->cm_id);

		kfree(rdma_handles[i]);
	}

	/* MR is set correctly iff rdma buffer and pd are correctly allocated */
	if (rdma_mr && !IS_ERR(rdma_mr)) {
		ib_dereg_mr(rdma_mr);
		ib_dma_unmap_single(rdma_pd->device, __rdma_sink_dma_addr,
				1 << (PAGE_SHIFT + MAX_ORDER - 1), DMA_FROM_DEVICE);
		free_pages((unsigned long)__rdma_sink_addr, MAX_ORDER - 1);
		ib_dealloc_pd(rdma_pd);
	}

	for (i = 0; i < send_buffer.nr_chunks; i++) {
		if (send_buffer.dma_addr_base[i]) {
			ib_dma_unmap_single(rdma_pd->device,
					send_buffer.dma_addr_base[i], RB_CHUNK_SIZE, DMA_TO_DEVICE);
		}
	}
	while (send_work_pool) {
		struct send_work *sw = send_work_pool;
		send_work_pool = sw->next;
		kfree(sw);
	}
	ring_buffer_destroy(&send_buffer);

	while (rdma_work_pool) {
		struct rdma_work *rw = rdma_work_pool;
		rdma_work_pool = rw->next;
		kfree(rw);
	}

	MSGPRINTK("Popcorn message layer over RDMA unloaded\n");
	return;
}

struct pcn_kmsg_transport transport_rdma = {
	.name = "rdma",
	.features = PCN_KMSG_FEATURE_RDMA,

	.get = rdma_kmsg_get,
	.put = rdma_kmsg_put,
	.stat = rdma_kmsg_stat,

	.send = rdma_kmsg_send,
	.post = rdma_kmsg_post,
	.done = rdma_kmsg_done,

	.pin_rdma_buffer = rdma_kmsg_pin_rdma_buffer,
	.unpin_rdma_buffer = rdma_kmsg_unpin_rdma_buffer,
	.rdma_write = rdma_kmsg_write,
	.rdma_read = rdma_kmsg_read,
};

int __init init_kmsg_rdma(void)
{
	int i;

	MSGPRINTK("\nLoading Popcorn messaging layer over RDMA...\n");

	/* Load node configuration */
        if (!load_config_file(config_file_path)) return -EINVAL;

	if (!identify_myself()) return -EINVAL;
	pcn_kmsg_set_transport(&transport_rdma);


	for (i = 0; i < MAX_NUM_NODES; i++) {
		struct rdma_handle *rh;
		rh = rdma_handles[i] = kzalloc(sizeof(struct rdma_handle), GFP_KERNEL);
		if (!rh) goto out_free;

		rh->nid = i;
		rh->state = RDMA_INIT;
		init_completion(&rh->cm_done);
	}

	if (__establish_connections())
		goto out_free;

	if (__setup_rdma_buffer(1))
		goto out_free;

	if (__setup_work_request_pools())
		goto out_free;

	broadcast_my_node_info(i);

	PCNPRINTK("Ready on InfiniBand RDMA\n");
	return 0;

out_free:
	exit_kmsg_rdma();
	return -EINVAL;
}

module_param(use_rb_thr, uint, 0644);
MODULE_PARM_DESC(use_rb_thr,
		"Threshold for using pre-allocated and pre-mapped ring buffer");

module_param_named(features, transport_rdma.features, ulong, 0644);
MODULE_PARM_DESC(use_rdma, "1: RDMA to transfer pages");

module_init(init_kmsg_rdma);
module_exit(exit_kmsg_rdma);
MODULE_LICENSE("GPL");
