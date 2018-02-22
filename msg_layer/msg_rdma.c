#include <linux/module.h>

#include <rdma/rdma_cm.h>

#include "common.h"

#define RDMA_PORT 11453
#define RDMA_ADDR_RESOLVE_TIMEOUT_MS 5000

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

	struct rdma_cm_id *cm_id;
	struct ib_pd *pd;
	struct ib_cq *cq;
	struct ib_qp *qp;
};

struct rdma_context {
	int wr_id;
	int wc_op;
};

static struct rdma_peer *peers[MAX_NUM_NODES] = { NULL };

void cq_comp_handler(struct ib_cq *cq, void *context)
{
	int ret;
	struct rdma_peer *peer = context;
	struct ib_wc wc;

	ib_req_notify_cq(peer->cq, IB_CQ_NEXT_COMP);
	while ((ret = ib_poll_cq(cq, 1, &wc)) > 0) {
		struct rdma_context *rc = (void *)(unsigned long)wc.wr_id;
		switch(rc->wc_op) {
		case IB_WC_RECV:
		case IB_WC_SEND:
		default:
			break;
		}
	}
}


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
			.cqe = 10, // DEPTHS
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
				.max_send_wr = 10,
				.max_recv_wr = 10,
				.max_send_sge = 4,
				.max_recv_sge = 4,
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


/**
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


/**
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

	PCNPRINTK("Popcorn messaging layer over RDMA is ready\n");
	return 0;

out_free:
	exit_kmsg_rdma();
	return -EINVAL;
}

module_init(init_kmsg_rdma);
module_exit(exit_kmsg_rdma);
MODULE_LICENSE("GPL");
