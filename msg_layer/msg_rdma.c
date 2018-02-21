#include <linux/module.h>

#include <rdma/rdma_cm.h>

#include "common.h"

#define RDMA_PORT 10101

struct rdma_peer {
	int id;
	enum {
		RDMA_INIT,
		RDMA_ADDR_RESOLVED,
		RDMA_ROUTE_RESOLVED,
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

static int __accept_client(struct rdma_cm_id *cm_id, struct rdma_cm_event *cm_event);

int cm_server_event_handler(struct rdma_cm_id *cm_id, struct rdma_cm_event *cm_event)
{
	int ret = 0;
	switch (cm_event->event) {
	case RDMA_CM_EVENT_CONNECT_REQUEST:
		ret = __accept_client(cm_id, cm_event);
		break;
	case RDMA_CM_EVENT_ESTABLISHED:
		break;
	case RDMA_CM_EVENT_CONNECT_RESPONSE:
	case RDMA_CM_EVENT_ADDR_RESOLVED:
	case RDMA_CM_EVENT_ROUTE_RESOLVED:
	case RDMA_CM_EVENT_ADDR_ERROR:
	case RDMA_CM_EVENT_ROUTE_ERROR:
	case RDMA_CM_EVENT_CONNECT_ERROR:
	case RDMA_CM_EVENT_UNREACHABLE:
	case RDMA_CM_EVENT_REJECTED:
	case RDMA_CM_EVENT_DISCONNECTED:
	case RDMA_CM_EVENT_DEVICE_REMOVAL:
	case RDMA_CM_EVENT_MULTICAST_JOIN:
	case RDMA_CM_EVENT_MULTICAST_ERROR:
	case RDMA_CM_EVENT_ADDR_CHANGE:
	case RDMA_CM_EVENT_TIMEWAIT_EXIT:
	default:
		printk("Unhandled event %d\n", cm_event->event);
		break;
	}
	return 0;
}

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
	case RDMA_CM_EVENT_CONNECT_REQUEST:
	case RDMA_CM_EVENT_CONNECT_RESPONSE:
	case RDMA_CM_EVENT_ADDR_ERROR:
	case RDMA_CM_EVENT_ROUTE_ERROR:
	case RDMA_CM_EVENT_CONNECT_ERROR:
	case RDMA_CM_EVENT_UNREACHABLE:
	case RDMA_CM_EVENT_REJECTED:
	case RDMA_CM_EVENT_DISCONNECTED:
	case RDMA_CM_EVENT_DEVICE_REMOVAL:
	case RDMA_CM_EVENT_MULTICAST_JOIN:
	case RDMA_CM_EVENT_MULTICAST_ERROR:
	case RDMA_CM_EVENT_ADDR_CHANGE:
	case RDMA_CM_EVENT_TIMEWAIT_EXIT:
	default:
		printk("Unhandled event %d\n", cm_event->event);
		break;
	}
	return 0;
}

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

static int __setup_pd_cq_qp(struct rdma_cm_id *cm_id, struct ib_pd **ret_pd, struct ib_cq **ret_cq, void *cq_context, struct ib_qp **ret_qp, void *qp_context, const char **step)
{
	int ret;
	struct ib_pd *pd;
	struct ib_cq *cq;
	struct ib_qp *qp;

	*step = "create pd";
	pd = ib_alloc_pd(cm_id->device);
	if (IS_ERR(pd)) {
		ret = PTR_ERR(pd);
		goto out_err;
	}
	*ret_pd = pd;

	*step = "create completion queue";
	{
		struct ib_cq_init_attr cq_attr = {
			.cqe = 10, // DEPTHS
			.comp_vector = 0,
		};

		cq = ib_create_cq(
				cm_id->device,cq_comp_handler, NULL, cq_context, &cq_attr);
		if (IS_ERR(cq)) {
			ret = PTR_ERR(cq);
			goto out_err;
		}
	}
	*ret_cq = cq;

	*step = "create queue pair";
	{
		struct ib_qp_init_attr qp_attr = {
			.event_handler = NULL, // qp_event_handler,
			.qp_context = qp_context,
			.cap = {
				.max_send_wr = 10,
				.max_recv_wr = 10,
				.max_send_sge = 4,
				.max_recv_sge = 4,
			},
			.sq_sig_type = IB_SIGNAL_REQ_WR,
			.qp_type = IB_QPT_RC,
			.send_cq = cq,
			.recv_cq = cq,
		};

		ret = rdma_create_qp(cm_id, pd, &qp_attr);
		if (ret) goto out_err;
	}
	*ret_qp = qp;

	*step = NULL;
	return 0;
out_err:
	return ret;
}

static int __accept_client(struct rdma_cm_id *cm_id, struct rdma_cm_event *cm_event)
{
	int ret;
	struct ib_pd *pd = NULL;
	struct ib_cq *cq = NULL;
	struct ib_qp *qp = NULL;
	const char *step;

	int peer_nid = *(int *)cm_event->param.conn.private_data;
	printk("Incoming request from %d\n", peer_nid);

	ret = __setup_pd_cq_qp(cm_id, &pd,
			&cq, peers + peer_nid, &qp, peers + peer_nid, &step);
	if (ret) goto out_err;

	step = "accept";
	{
		struct rdma_conn_param conn_param = {
			.responder_resources = 0,
			.initiator_depth = 0,
		};
		ret = rdma_accept(cm_id, &conn_param);
		if (ret) goto out_err;

		/*
		ret = wait_for_completion_interruptible(&comp);
		if (ret) goto out_err;
		*/
	}
	return 0;
out_err:
	if (qp) ib_destroy_qp(qp);
	if (cq) ib_destroy_cq(cq);
	if (pd) ib_dealloc_pd(pd);
	if (!IS_ERR(cm_id)) rdma_destroy_id(cm_id);
	return ret;
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

		ret = rdma_resolve_addr(
				peer->cm_id, NULL,(struct sockaddr *)&addr, 5000);
		if (ret) goto out_err;

		ret = wait_for_completion_interruptible(&peer->cm_done);
		if (ret || peer->state != RDMA_ADDR_RESOLVED) goto out_err;
	}

	step = "resolve path";
	ret = rdma_resolve_route(peer->cm_id, 5000);
	if (ret) goto out_err;
	ret = wait_for_completion_interruptible(&peer->cm_done);
	if (ret || peer->state != RDMA_ROUTE_RESOLVED) goto out_err;

	ret = __setup_pd_cq_qp(peer->cm_id, &peer->pd,
			&peer->cq, peer, &peer->qp, peer, &step);
	if (ret) goto out_err;

	step = "connect";
	{
		struct rdma_conn_param conn_param = {
			.responder_resources = 0,
			.initiator_depth = 0,
			.private_data = &peer->id,
			.private_data_len = sizeof(int),
		};

		ret = rdma_connect(peer->cm_id, &conn_param);
		if (ret) goto out_err;
		ret = wait_for_completion_interruptible(&peer->cm_done);
		if (ret || peer->state != RDMA_CONNECTED) goto out_err;
	}

	return 0;

out_err:
	PCNPRINTK_ERR("Cannot %s, %pI4, %d\n", step, ip_table + nid, ret);
	return ret;
}

static int __establish_connections(void)
{
	int i, ret;

	for (i = 0; i < my_nid; i++) {
		ret = __connect_to_server(i);
		if (ret) return ret;
	}

	ret = __listen_to_connection();
	if (ret) return ret;

	return 0;
}


void __exit exit_kmsg_rdma(void)
{
	int i;
	for (i = 0; i < MAX_NUM_NODES; i++) {
		struct rdma_peer *peer = peers[i];
		if (!peer) continue;

		if (peer->cm_id && !IS_ERR(peer->cm_id)) {
			rdma_destroy_id(peer->cm_id);
		}
		if (peer->pd && !IS_ERR(peer->pd)) {
			ib_dealloc_pd(peer->pd);
		}
		kfree(peers[i]);
	}
	printk("Unloaded completely\n");
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

		peer->id = i;
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
