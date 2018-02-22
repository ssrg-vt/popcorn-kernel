#include <linux/module.h>

#include <linux/vmalloc.h>
#include <rdma/rdma_cm.h>

#include "common.h"

#define RDMA_PORT 11453
#define RDMA_ADDR_RESOLVE_TIMEOUT_MS 5000

#define MAX_SEND_DEPTH	128
#define MAX_RECV_DEPTH	128

struct recv_work {
	struct ib_sge sgl;
	struct ib_recv_wr rwr;
	u64 dma_addr;
};

struct rdma_peer {
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

	unsigned int send_ring_head;
	unsigned int send_ring_tail;
	void *send_ring;
	u64 send_ring_dma_addr;

	struct recv_work *recv_works;
	void *recv_buffer;
	u64 recv_buffer_dma_addr;

	struct rdma_cm_id *cm_id;
	struct ib_pd *pd;
	struct ib_cq *cq;
	struct ib_qp *qp;
	struct ib_mr *mr;
};


static struct rdma_peer *peers[MAX_NUM_NODES] = { NULL };

static void __process_recv(struct ib_qp *qp, struct recv_work *rw)
{
	struct ib_recv_wr *bad_wr;
	int ret = ib_post_recv(qp, &rw->rwr, &bad_wr);
	BUG_ON(ret || bad_wr);
	printk("recv something\n");
}

static void __process_sent(struct ib_qp *qp)
{
	printk("sent something\n");
}

void cq_comp_handler(struct ib_cq *cq, void *context)
{
	int ret;
	struct rdma_peer *peer = context;
	struct ib_wc wc;

	ib_req_notify_cq(peer->cq, IB_CQ_NEXT_COMP);
	while ((ret = ib_poll_cq(cq, 1, &wc)) > 0) {
		struct recv_work *rw = (void *)wc.wr_id;
		printk("%s: %llx %d %d\n", __func__, wc.wr_id, wc.opcode, wc.status);
		switch(wc.opcode) {
		case IB_WC_RECV:
			__process_recv(wc.qp, rw);
			break;
		case IB_WC_SEND:
			__process_sent(wc.qp);
			break;
		case IB_WC_RDMA_WRITE:
		case IB_WC_RDMA_READ:
		default:
			printk("Unknown completion op %d\n", wc.opcode);
			break;
		}
	}
}


/****************************************************************************
 * Setup connections
 */
static int __setup_pd_cq_qp(struct rdma_peer *peer)
{
	int ret;

	/* Create pd */
	peer->pd = ib_alloc_pd(peer->cm_id->device);
	if (IS_ERR(peer->pd)) {
		ret = PTR_ERR(peer->pd);
		goto out_err;
	}

	/* create completion queue */
	{
		struct ib_cq_init_attr cq_attr = {
			.cqe = MAX_SEND_DEPTH + MAX_RECV_DEPTH,
			.comp_vector = 0,
		};

		peer->cq = ib_create_cq(
				peer->cm_id->device, cq_comp_handler, NULL, peer, &cq_attr);
		if (IS_ERR(peer->cq)) {
			ret = PTR_ERR(peer->cq);
			goto out_err;
		}
	}

	/* create queue pair */
	{
		struct ib_qp_init_attr qp_attr = {
			.event_handler = NULL, // qp_event_handler,
			.qp_context = peer,
			.cap = {
				.max_send_wr = MAX_SEND_DEPTH,
				.max_recv_wr = MAX_RECV_DEPTH,
				.max_send_sge = PCN_KMSG_MAX_SIZE >> PAGE_SHIFT,
				.max_recv_sge = PCN_KMSG_MAX_SIZE >> PAGE_SHIFT,
			},
			.sq_sig_type = IB_SIGNAL_REQ_WR,
			.qp_type = IB_QPT_RC,
			.send_cq = peer->cq,
			.recv_cq = peer->cq,
		};

		ret = rdma_create_qp(peer->cm_id, peer->pd, &qp_attr);
		if (ret) goto out_err;
		peer->qp = peer->cm_id->qp;
	}
	return 0;

out_err:
	return ret;
}

static int __setup_buffers_and_pools(struct rdma_peer *peer)
{
	int ret = 0, i;
	struct ib_mr *mr;
	u64 dma_addr;
	void *recv_buffer = NULL;
	struct recv_work *rws = NULL;
	const size_t buffer_size = PCN_KMSG_MAX_SIZE * MAX_RECV_DEPTH;

	recv_buffer = vmalloc(buffer_size);
	if (!recv_buffer) {
		return -ENOMEM;
	}
	rws = kmalloc(sizeof(*rws) * MAX_RECV_DEPTH, GFP_KERNEL);
	if (!rws) {
		ret = -ENOMEM;
		goto out;
	}

	mr = ib_alloc_mr(peer->pd, IB_MR_TYPE_MEM_REG, buffer_size >> 12);
	if (IS_ERR(mr)) {
		ret = PTR_ERR(mr);
		goto out;
	}
	dma_addr = ib_dma_map_single(
			peer->cm_id->device, recv_buffer, buffer_size, DMA_FROM_DEVICE);
	ret = ib_dma_mapping_error(peer->cm_id->device, dma_addr);
	if (ret) goto out_dereg;

	for (i = 0; i < MAX_RECV_DEPTH; i++) {
		struct ib_recv_wr *rwr, *bad_wr;
		struct ib_sge *sgl;
		struct recv_work *rw = rws + i;

		rw->dma_addr = dma_addr + PCN_KMSG_MAX_SIZE * i;

		sgl = &rw->sgl;
		sgl->length = PCN_KMSG_MAX_SIZE;
		sgl->lkey = peer->pd->local_dma_lkey;
		sgl->addr = rw->dma_addr;

		rwr = &rw->rwr;
		rwr->sg_list = sgl;
		rwr->num_sge = 1;
		rwr->next = NULL;
		rwr->wr_id = (u64)rw;

		ret = ib_post_recv(peer->qp, rwr, &bad_wr);
		if (ret) goto out_dereg;
	}
	peer->mr = mr;
	peer->recv_works = rws;
	peer->recv_buffer = recv_buffer;
	peer->recv_buffer_dma_addr = dma_addr;

	return ret;

out_dereg:
	ib_dereg_mr(mr);

out:
	if (recv_buffer) vfree(recv_buffer);
	if (rws) vfree(rws);
	return ret;
}


/****************************************************************************
 * Client-side connection handling
 */
int cm_client_event_handler(struct rdma_cm_id *cm_id, struct rdma_cm_event *cm_event)
{
	struct rdma_peer *peer = cm_id->context;

	switch (cm_event->event) {
	case RDMA_CM_EVENT_ADDR_RESOLVED:
		peer->state = RDMA_ADDR_RESOLVED;
		complete(&peer->cm_done);
		break;
	case RDMA_CM_EVENT_ROUTE_RESOLVED:
		peer->state = RDMA_ROUTE_RESOLVED;
		complete(&peer->cm_done);
		break;
	case RDMA_CM_EVENT_ESTABLISHED:
		peer->state = RDMA_CONNECTED;
		complete(&peer->cm_done);
		break;
	case RDMA_CM_EVENT_DISCONNECTED:
		MSGPRINTK("Disconnected from %d\n", peer->nid);
		/* TODO deallocate associated resources */
		break;
	case RDMA_CM_EVENT_REJECTED:
	case RDMA_CM_EVENT_CONNECT_ERROR:
		complete(&peer->cm_done);
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
	struct rdma_peer *peer = peers[nid];

	step = "create rdma id";
	peer->cm_id = rdma_create_id(&init_net,
			cm_client_event_handler, peer, RDMA_PS_IB, IB_QPT_RC);
	if (IS_ERR(peer->cm_id)) goto out_err;

	step = "resolve peer address";
	{
		struct sockaddr_in addr = {
			.sin_family = AF_INET,
			.sin_port = htons(RDMA_PORT),
			.sin_addr.s_addr = ip_table[nid],
		};

		ret = rdma_resolve_addr(peer->cm_id,NULL,
				(struct sockaddr *)&addr, RDMA_ADDR_RESOLVE_TIMEOUT_MS);
		if (ret) goto out_err;

		ret = wait_for_completion_interruptible(&peer->cm_done);
		if (ret || peer->state != RDMA_ADDR_RESOLVED) goto out_err;
	}

	step = "resolve path";
	ret = rdma_resolve_route(peer->cm_id, RDMA_ADDR_RESOLVE_TIMEOUT_MS);
	if (ret) goto out_err;
	ret = wait_for_completion_interruptible(&peer->cm_done);
	if (ret || peer->state != RDMA_ROUTE_RESOLVED) goto out_err;

	step = "setup ib";
	ret = __setup_pd_cq_qp(peer);
	if (ret) goto out_err;

	step = "setup buffers and pools";
	ret = __setup_buffers_and_pools(peer);
	if (ret) goto out_err;

	step = "connect";
	{
		struct rdma_conn_param conn_param = {
			.responder_resources = 0,
			.initiator_depth = 0,
			.private_data = &my_nid,
			.private_data_len = sizeof(my_nid),
		};

		ret = rdma_connect(peer->cm_id, &conn_param);
		if (ret) goto out_err;
		ret = wait_for_completion_interruptible(&peer->cm_done);
		if (ret || peer->state != RDMA_CONNECTED) goto out_err;
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
	int peer_nid = *(int *)cm_event->param.conn.private_data;
	struct rdma_peer *peer = peers[peer_nid];

	struct rdma_conn_param conn_param = {
		.responder_resources = 0,
		.initiator_depth = 0,
	};
	int ret;

	cm_id->context = peer;
	peer->cm_id = cm_id;
	peer->state = RDMA_ROUTE_RESOLVED;

	ret = __setup_pd_cq_qp(peer);
	if (ret) return ret;

	ret = __setup_buffers_and_pools(peer);
	if (ret) return ret;

	ret = rdma_accept(cm_id, &conn_param);
	if (ret) return ret;

	return 0;
}

static int __on_client_connected(struct rdma_cm_id *cm_id, struct rdma_cm_event *cm_event)
{
	struct rdma_peer *peer = cm_id->context;
	peer->state = RDMA_CONNECTED;
	set_popcorn_node_online(peer->nid, true);

	MSGPRINTK("Connected to %d\n", peer->nid);
	return 0;
}

static int __on_client_disconnected(struct rdma_cm_id *cm_id, struct rdma_cm_event *cm_event)
{
	struct rdma_peer *peer = cm_id->context;
	peer->state = RDMA_INIT;
	set_popcorn_node_online(peer->nid, false);

	MSGPRINTK("Disconnected from %d\n", peer->nid);
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
	peers[my_nid]->cm_id = cm_id;

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

static void __test_send(void)
{
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
		struct rdma_peer *peer = peers[i];
		set_popcorn_node_online(i, false);

		if (!peer) continue;

		if (peer->recv_buffer) {
			ib_dma_unmap_single(peer->cm_id->device, peer->recv_buffer_dma_addr,
					PCN_KMSG_MAX_SIZE * MAX_RECV_DEPTH, DMA_FROM_DEVICE);
			vfree(peer->recv_buffer);
			kfree(peer->recv_works);
			ib_dereg_mr(peer->mr);
		}

		if (peer->qp && !IS_ERR(peer->qp)) rdma_destroy_qp(peer->cm_id);
		if (peer->cq && !IS_ERR(peer->cq)) ib_destroy_cq(peer->cq);
		if (peer->pd && !IS_ERR(peer->pd)) ib_dealloc_pd(peer->pd);
		if (peer->cm_id && !IS_ERR(peer->cm_id)) rdma_destroy_id(peer->cm_id);

		kfree(peers[i]);
	}
	MSGPRINTK("Popcorn message layer over RDMA unloaded\n");
	return;
}

int __init init_kmsg_rdma(void)
{
	int i;

	MSGPRINTK("Loading Popcorn messaging layer over RDMA...\n");
	if (!identify_myself()) return -EINVAL;

	for (i = 0; i < MAX_NUM_NODES; i++) {
		struct rdma_peer *peer;
		peer = peers[i] = kzalloc(sizeof(struct rdma_peer), GFP_KERNEL);
		if (!peer) goto out_free;

		peer->nid = i;
		peer->state = RDMA_INIT;
		init_completion(&peer->cm_done);
	}

	if (__establish_connections()) {
		goto out_free;
	}

	__test_send();

	PCNPRINTK("Popcorn messaging layer over RDMA is ready\n");
	return 0;

out_free:
	exit_kmsg_rdma();
	return -EINVAL;
}

module_init(init_kmsg_rdma);
module_exit(exit_kmsg_rdma);
MODULE_LICENSE("GPL");
