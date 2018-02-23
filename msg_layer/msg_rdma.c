#include <linux/module.h>

#include <linux/vmalloc.h>
#include <rdma/rdma_cm.h>

#include "common.h"

#define RDMA_PORT 11453
#define RDMA_ADDR_RESOLVE_TIMEOUT_MS 5000

#define MAX_SEND_DEPTH	((1 << (PAGE_SHIFT + MAX_ORDER - 1)) / PCN_KMSG_MAX_SIZE)
#define MAX_RECV_DEPTH	((1 << (PAGE_SHIFT + MAX_ORDER - 1)) / PCN_KMSG_MAX_SIZE)

struct recv_work {
	struct ib_sge sgl;
	struct ib_recv_wr wr;
	u64 dma_addr;
	void *buffer;
};

struct send_work {
	struct ib_sge sgl;
	struct ib_send_wr wr;
	u64 dma_addr;
	void *buffer;
	struct send_work *next;
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

	spinlock_t send_work_pool_lock;
	struct send_work *send_work_pool;
	void *send_buffer;
	u64 send_buffer_dma_addr;

	struct recv_work *recv_works;
	void *recv_buffer;
	u64 recv_buffer_dma_addr;

	void *rdma_sink;
	u64 rdma_sink_dma_addr;

	struct rdma_cm_id *cm_id;
	struct ib_device *device;
	struct ib_pd *pd;
	struct ib_cq *cq;
	struct ib_qp *qp;
	struct ib_mr *mr;
};

static struct rdma_handle *rdma_handles[MAX_NUM_NODES] = { NULL };

static inline int __post_send(struct rdma_handle *rh, u64 dma_addr, size_t size, u64 wr_id)
{
	struct ib_send_wr *bad_wr = NULL;
	struct ib_sge sgl = {
		.addr = dma_addr,
		.length = size,
		.lkey = rh->pd->local_dma_lkey,
	};
	struct ib_send_wr wr = {
		.next = NULL,
		.wr_id = wr_id,
		.sg_list = &sgl,
		.num_sge = 1,
		.opcode = IB_WR_SEND, //IB_WR_SEND_WITH_IMM,
		.send_flags = IB_SEND_SIGNALED,
	};
	int ret;

	ret = ib_post_send(rh->qp, &wr, &bad_wr);
	if (ret) return ret;
	if (bad_wr) {
		printk("failed to send, %d %p\n", ret, bad_wr);
		return -EINVAL;
	}
	return 0;
}

static int __send_to(int to_nid, void *payload, size_t size)
{
	struct rdma_handle *rh = rdma_handles[to_nid];
	struct ib_device *dev = rh->device;
	u64 dma_addr;
	int ret;
	DECLARE_COMPLETION_ONSTACK(comp);

	dma_addr = ib_dma_map_single(dev, payload, size, DMA_TO_DEVICE);
	ret = ib_dma_mapping_error(dev, dma_addr);
	if (ret) {
		printk("mapping fail %d\n", ret);
		return -ENODEV;
	} else {
		printk("mapped %p to %llx\n", payload, dma_addr);
	}
	ret = __post_send(rh, dma_addr, size, (u64)&comp);
	if (ret) goto out;
	ret = wait_for_completion_timeout(&comp, 10 * HZ);
	if (!ret) ret = -EAGAIN;
	ret = 0;

out:
	ib_dma_unmap_single(dev, dma_addr, size, DMA_TO_DEVICE);
	return ret;
}


static struct ib_mr *__register_mr(struct rdma_handle *rh)
{
	DECLARE_COMPLETION_ONSTACK(comp);
	struct ib_mr *mr = NULL;
	struct ib_send_wr *bad_wr = NULL;
	struct ib_reg_wr reg_wr = {
		.wr = {
			.opcode = IB_WR_REG_MR,
			.send_flags = IB_SEND_SIGNALED,
			.wr_id = (u64)&comp,
		},
		.access =	IB_ACCESS_REMOTE_READ |
					IB_ACCESS_REMOTE_WRITE |
					IB_ACCESS_LOCAL_WRITE |
					IB_ACCESS_REMOTE_ATOMIC,
	};
	struct scatterlist sg = {};
	int ret;

	mr = ib_alloc_mr(rh->pd, IB_MR_TYPE_MEM_REG, 1 << (MAX_ORDER - 1));
	BUG_ON(!mr);
	sg_dma_address(&sg) = rh->rdma_sink_dma_addr;
	sg_dma_len(&sg) = 1 << (PAGE_SHIFT + MAX_ORDER - 1);

	ret = ib_map_mr_sg(mr, &sg, 1, PAGE_SIZE);
	if (ret != 1) {
		printk("Cannot map sg, %d\n", ret);
		goto out_dereg;
	}

	reg_wr.mr = mr;
	reg_wr.key = mr->lkey;

	ret = ib_post_send(rh->qp, &reg_wr.wr, &bad_wr);
	if (ret || bad_wr) {
		printk("regmr: %d %p\n", ret, bad_wr);
		goto out_dereg;
	}
	ret = wait_for_completion_timeout(&comp, 10 * HZ);
	if (!ret) {
		printk("Time out to register mr\n");
		goto out_dereg;
	}
	printk("lkey: %x, rkey: %x, length: %x\n", mr->lkey, mr->rkey, mr->length);
	return mr;

out_dereg:
	ib_dereg_mr(mr);
	return NULL;
}

struct rdma_request {
	int nid;
	u32 rkey;
	u64 addr;
	size_t length;
};

static void __test_rdma(int to_nid)
{
	struct rdma_handle *rh = rdma_handles[to_nid];
	struct rdma_request req;
	struct ib_mr *mr;
	DECLARE_COMPLETION_ONSTACK(comp);
	int ret;

	mr = __register_mr(rh);
	if (!mr) return;

	req.nid = my_nid;
	req.rkey = mr->rkey;
	req.addr = rh->rdma_sink_dma_addr;
	req.length = PAGE_SIZE;

	printk("RDMA %x %llx %ld\n", req.rkey, req.addr, req.length);
	memset(rh->rdma_sink, 0x00, PAGE_SIZE);

	ret = __send_to(to_nid, &req, sizeof(req));
	if (ret) goto out;

	{
		int retry = 0;
		char *p = rh->rdma_sink;
		while (retry++ < 100) {
			if (p[PAGE_SIZE-1]) break;
			msleep(1000);
		}
		if (retry >= 10) {
			printk("timed out\n");
		}
		printk("%c %c %c %c\n", p[0], p[1], p[PAGE_SIZE-2], p[PAGE_SIZE-1]);
	}

out:
	ib_dereg_mr(mr);
}

static int sent = 0;
static void __perform_rdma(struct ib_wc *wc, struct recv_work *rw)
{
	struct rdma_request *req = rw->buffer;
	struct ib_sge sgl = {
		.addr = 0,
		.length = 0,
		.lkey = 0,
	};
	struct ib_rdma_wr wr = {
		.wr = {
			.next = NULL,
			.wr_id = 0,
			.sg_list = &sgl,
			.num_sge = 1,
			.opcode = IB_WR_RDMA_WRITE, // IB_WR_RDMA_WRITE_WITH_IMM;
			.send_flags = IB_SEND_SIGNALED,
		},
		.remote_addr = req->addr,
		.rkey = req->rkey,
	};
	struct ib_send_wr *bad_wr = NULL;

	char *payload = (void *)__get_free_page(GFP_ATOMIC);
	const int size = PAGE_SIZE;
	u64 dma_addr;
	int ret;
	BUG_ON(!payload);
	memset(payload, 'a' + sent++, PAGE_SIZE);

	dma_addr = ib_dma_map_single(wc->qp->device, payload, size, DMA_TO_DEVICE);
	ret = ib_dma_mapping_error(wc->qp->device, dma_addr);
	BUG_ON(ret);

	wr.wr.wr_id = (u64)payload;

	sgl.addr = dma_addr;
	sgl.length = size;
	sgl.lkey = wc->qp->pd->local_dma_lkey;

	ret = ib_post_send(wc->qp, &wr.wr, &bad_wr);
	if (ret || bad_wr) {
		printk("Cannot post rdma write, %d, %p\n", ret, bad_wr);
		free_page((unsigned long)payload);
	} else {
		printk("post rdma write\n");
	}
	ib_dma_unmap_single(wc->qp->device, dma_addr, size, DMA_TO_DEVICE);
}

/****************************************************************************
 * Event handlers
 */
static void __process_recv(struct ib_wc *wc)
{
	struct recv_work *rw = (void *)wc->wr_id;
	struct ib_recv_wr *bad_wr = NULL;
	int ret;

	printk("recv %x %u\n", wc->wc_flags, wc->byte_len);

	__perform_rdma(wc, rw);

	/* Put back the receive work */
	ret = ib_post_recv(wc->qp, &rw->wr, &bad_wr);
	BUG_ON(ret || bad_wr);
}

static void __process_comp_wakeup(struct ib_wc *wc, const char *msg)
{
	struct completion *comp = (void *)wc->wr_id;
	complete(comp);
	if (msg) printk(msg);
}

void cq_comp_handler(struct ib_cq *cq, void *context)
{
	int ret;
	struct ib_wc wc;

	while ((ret = ib_poll_cq(cq, 1, &wc)) > 0) {
		if (wc.opcode < 0 || wc.status) {
			printk("abnormal status %d with %d\n", wc.status, wc.opcode);
			continue;
		}
		switch(wc.opcode) {
		case IB_WC_RECV:
			__process_recv(&wc);
			break;
		case IB_WC_SEND:
			__process_comp_wakeup(&wc, "message sent\n");
			break;
		case IB_WC_REG_MR:
			__process_comp_wakeup(&wc, "mr reg completed\n");
			break;
		case IB_WC_RDMA_WRITE:
		case IB_WC_RDMA_READ:
			printk("rdma completed\n");
			free_page(wc.wr_id);
			break;
		default:
			printk("Unknown completion op %d\n", wc.opcode);
			break;
		}
	}
	ib_req_notify_cq(cq, IB_CQ_NEXT_COMP);
}


/****************************************************************************
 * Setup connections
 */
static int __setup_pd_cq_qp(struct rdma_handle *rh)
{
	int ret;

	/* Create pd */
	rh->pd = ib_alloc_pd(rh->device);
	if (IS_ERR(rh->pd)) {
		ret = PTR_ERR(rh->pd);
		goto out_err;
	}

	/* create completion queue */
	{
		struct ib_cq_init_attr cq_attr = {
			.cqe = MAX_SEND_DEPTH + MAX_RECV_DEPTH,
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
				.max_recv_wr = MAX_RECV_DEPTH,
				.max_send_sge = PCN_KMSG_MAX_SIZE >> PAGE_SHIFT,
				.max_recv_sge = PCN_KMSG_MAX_SIZE >> PAGE_SHIFT,
			},
			.sq_sig_type = IB_SIGNAL_REQ_WR,
			.qp_type = IB_QPT_RC,
			.send_cq = rh->cq,
			.recv_cq = rh->cq,
		};

		ret = rdma_create_qp(rh->cm_id, rh->pd, &qp_attr);
		if (ret) goto out_err;
		rh->qp = rh->cm_id->qp;
	}
	return 0;

out_err:
	return ret;
}

static int __setup_buffers_and_pools(struct rdma_handle *rh)
{
	int ret = 0, i;
	u64 dma_addr;
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
		struct ib_recv_wr *wr, *bad_wr = NULL;
		struct ib_sge *sgl;

		rw->dma_addr = dma_addr + PCN_KMSG_MAX_SIZE * i;
		rw->buffer = recv_buffer + PCN_KMSG_MAX_SIZE * i;

		sgl = &rw->sgl;
		sgl->lkey = rh->pd->local_dma_lkey;
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

	/* RDMA buffers */
	rh->rdma_sink = (void *)__get_free_pages(GFP_KERNEL, MAX_ORDER - 1);
	BUG_ON(!rh->rdma_sink);
	rh->rdma_sink_dma_addr = ib_dma_map_single(
			rh->device, rh->rdma_sink, 1 << (PAGE_SHIFT + MAX_ORDER - 1),
			DMA_FROM_DEVICE);
	ret = ib_dma_mapping_error(rh->device, rh->rdma_sink_dma_addr);
	BUG_ON(ret);

	return ret;

out_free:
	if (recv_buffer) kfree(recv_buffer);
	if (rws) kfree(rws);
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

	step = "resolve rh address";
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

	step = "resolve path";
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
			.responder_resources = 0,
			.initiator_depth = 0,
			.private_data = &my_nid,
			.private_data_len = sizeof(my_nid),
		};

		ret = rdma_connect(rh->cm_id, &conn_param);
		if (ret) goto out_err;
		ret = wait_for_completion_interruptible(&rh->cm_done);
		if (ret) goto out_err;
		if (rh->state != RDMA_CONNECTED) {
			ret = -EAGAIN;
			goto out_err;
		}
	}

	set_popcorn_node_online(nid, true);
	MSGPRINTK("Connected to %d\n", nid);
	return 0;

out_err:
	PCNPRINTK_ERR("Unable to %s, %pI4, %d\n", step, ip_table + nid, ret);
	return ret;
}


/****************************************************************************
 * Server-side connection handling
 */
static int __accept_client(struct rdma_cm_id *cm_id, struct rdma_cm_event *cm_event)
{
	int rh_nid = *(int *)cm_event->param.conn.private_data;
	struct rdma_handle *rh = rdma_handles[rh_nid];
	struct rdma_conn_param conn_param = {
		.responder_resources = 0,
		.initiator_depth = 0,
	};
	int ret;

	cm_id->context = rh;
	rh->cm_id = cm_id;
	rh->device = cm_id->device;
	rh->state = RDMA_ROUTE_RESOLVED;

	ret = __setup_pd_cq_qp(rh);
	if (ret) return ret;

	ret = __setup_buffers_and_pools(rh);
	if (ret) return ret;

	ret = rdma_accept(cm_id, &conn_param);
	if (ret) return ret;

	return 0;
}

static int __on_client_connected(struct rdma_cm_id *cm_id, struct rdma_cm_event *cm_event)
{
	struct rdma_handle *rh = cm_id->context;
	rh->state = RDMA_CONNECTED;
	set_popcorn_node_online(rh->nid, true);

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
		ret = __accept_client(cm_id, cm_event);
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

	for (i = 0; i < my_nid; i++) {
		ret = __connect_to_server(i);
		if (ret) return ret;
	}

	for (i = my_nid + 1; i < MAX_NUM_NODES; i++) {
		while (!get_popcorn_node_online(i)) {
			msleep(100);
			io_schedule();
		}
	}

	return 0;
}

void __exit exit_kmsg_rdma(void)
{
	int i;
	for (i = 0; i < MAX_NUM_NODES; i++) {
		struct rdma_handle *rh = rdma_handles[i];
		set_popcorn_node_online(i, false);
		if (!rh) continue;

		if (rh->rdma_sink) {
			ib_dma_unmap_single(rh->device, rh->rdma_sink_dma_addr,
					1 << (PAGE_SHIFT + MAX_ORDER - 1), DMA_FROM_DEVICE);
			free_pages((unsigned long)rh->rdma_sink, MAX_ORDER - 1);
		}

		if (rh->recv_buffer) {
			ib_dma_unmap_single(rh->device, rh->recv_buffer_dma_addr,
					PCN_KMSG_MAX_SIZE * MAX_RECV_DEPTH, DMA_FROM_DEVICE);
			kfree(rh->recv_buffer);
			kfree(rh->recv_works);
		}

		if (rh->qp && !IS_ERR(rh->qp)) rdma_destroy_qp(rh->cm_id);
		if (rh->cq && !IS_ERR(rh->cq)) ib_destroy_cq(rh->cq);
		if (rh->pd && !IS_ERR(rh->pd)) ib_dealloc_pd(rh->pd);
		if (rh->cm_id && !IS_ERR(rh->cm_id)) rdma_destroy_id(rh->cm_id);

		kfree(rdma_handles[i]);
	}
	MSGPRINTK("Popcorn message layer over RDMA unloaded\n");
	return;
}

int __init init_kmsg_rdma(void)
{
	int i;

	MSGPRINTK("\nLoading Popcorn messaging layer over RDMA...\n");
	if (!identify_myself()) return -EINVAL;

	for (i = 0; i < MAX_NUM_NODES; i++) {
		struct rdma_handle *rh;
		rh = rdma_handles[i] = kzalloc(sizeof(struct rdma_handle), GFP_KERNEL);
		if (!rh) goto out_free;

		rh->nid = i;
		rh->state = RDMA_INIT;
		init_completion(&rh->cm_done);
	}

	if (__establish_connections()) {
		goto out_free;
	}

	if (my_nid == 1) {
		for (i = 0; i < 26; i++) {
			__test_rdma(0);
		}
	}

	PCNPRINTK("Popcorn messaging layer over RDMA is ready\n");
	return 0;

out_free:
	exit_kmsg_rdma();
	return -EINVAL;
}

module_init(init_kmsg_rdma);
module_exit(exit_kmsg_rdma);
MODULE_LICENSE("GPL");
