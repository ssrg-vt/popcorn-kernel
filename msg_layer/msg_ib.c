/*
 * pcn_kmesg.c - Kernel Module for Popcorn Messaging Layer over Socket
 * Author: Ho-Ren(Jack) Chuang
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/semaphore.h>
#include <linux/file.h>
#include <linux/ktime.h>
#include <linux/fdtable.h>
#include <linux/time.h>
#include <asm/atomic.h>
#include <linux/atomic.h>
#include <linux/completion.h>
#include <linux/errno.h>
#include <linux/cpumask.h>
#include <linux/sched.h>
#include <linux/vmalloc.h>

/* net */
#include <linux/net.h>
#include <net/sock.h>
#include <linux/tcp.h>
#include <linux/in.h>
#include <asm/uaccess.h>
#include <linux/socket.h>

/* geting host ip */
#include <linux/netdevice.h>
#include <linux/inetdevice.h>
#include <linux/inet.h>

/* pci */
#include <linux/pci.h>
#include <asm/pci.h>

//#include <linux/pcn_kmsg.h>

/* rdma */
#include <rdma/ib_verbs.h>
#include <rdma/rdma_cm.h>

#include "common.h"
//#include "msg_ib_handlers.c"

/* page */
#include <linux/pagemap.h>

/* Jack
 *  mssg layer multi-version
 *  msg sent to data_sock[conn_no] according to dest_cpu
 *  TODO:
 *			replace msleep with wait/complete 
 *			replace addr lookup with Sang-Hoon's implementation
 *			test htonll and remove #define htonll(x) cpu_to_be64((x))
 *			test ntohll and remove #define ntohll(x) cpu_to_be64((x))
 * 			make parameters in __krping_create_qp global
 *			cb -> _cb
 *			doesn't check RW size
 */

// features been developed
#define CONFIG_FARM 1
#define DEMON_POST_RECV_WCS 0

#define POPCORN_DEBUG_MSG_IB 0
#if POPCORN_DEBUG_MSG_IB
#define EXP_LOG(...) printk(__VA_ARGS__)
#define DEVLOP_LOG(...) printk(__VA_ARGS__)
#define MSG_RDMA_PRK(...) printk(__VA_ARGS__)
#define KRPRINT_INIT(...) printk(__VA_ARGS__)
#define MSG_SYNC_PRK(...) printk(__VA_ARGS__)
#define DEBUG_LOG(...) printk(__VA_ARGS__)
#define DEBUG_LOG_V(...) printk(__VA_ARGS__)
/* for RW data correctness sanity check */
#define CHECK_LOG(...) printk(__VA_ARGS__)
#else
#define EXP_LOG(...)
#define DEVLOP_LOG(...)
#define MSG_RDMA_PRK(...)
#define KRPRINT_INIT(...)
#define MSG_SYNC_PRK(...)
#define DEBUG_LOG(...)
#define DEBUG_LOG_V(...)
#define CHECK_LOG(...)
#endif 

#define htonll(x) cpu_to_be64((x))
#define ntohll(x) cpu_to_be64((x)) 

#define PORT 1000
#define MAX_RDMA_SIZE 4*1024*1024 // MAX READ/WRITE performing SIZE

/*
 * HW info:
 * attr.cqe = cb->txdepth * 8;
 * - cq entries - indicating we want room for ten entries on the queue.
 *	This number should be set large enough that the queue isn’t overrun.
 */
/* - recv - */
#define MAX_RECV_WR 15000	// important!! If only sender crash, must check it.

/* - send - */
#define RPING_SQ_DEPTH 128	// sender depth (txdepth)
#define SEND_DEPTH 8
// Attention:
// [x.xxxxxx] mlx5_core 0000:01:00.0: swiotlb buffer is full (sz: 8388608 bytes)

/* for prevent from long staying in INT handler, __krping_cq_event_handler() */
#define RECV_WQ_THRESHOLD 10
#define LISTEN_BACKLOG 99

#define INT_MASK 0

/* ib configurations */
int g_conn_responder_resuorces = 1;
int g_conn_initiator_depth = 1;
int g_conn_retry_count = 10;

/* IB runtime status */
#define IDLE 1
#define CONNECT_REQUEST 2
#define ADDR_RESOLVED 3
#define ROUTE_RESOLVED 4
#define CONNECTED 5
#define RDMA_READ_COMPLETE 6
#define RDMA_WRITE_COMPLETE 7
#define RDMA_SEND_COMPLETE 8
#define ERROR 9

#ifdef CONFIG_POPCORN_DEBUG_MSG_LAYER_VERBOSE
/* For debuging */
atomic_t g_rw_ticket;
atomic_t g_send_ticket;
atomic_t g_recv_ticket;
#endif

#define MAX_MSG_LENGTH PCN_KMSG_LONG_PAYLOAD_SIZE

/* rdma RW testing buffers */
extern char* dummy_act_buf;
extern char* dummy_pass_buf;

/*
 * rq_wr -> wc
 */
struct wc_struct {
	struct list_head list;
	struct pcn_kmsg_message *element_addr;
	struct ib_sge *recv_sgl;
	struct ib_recv_wr *rq_wr;
};

#ifdef CONFIG_POPCORN_DEBUG_MSG_LAYER_VERBOSE
/* IB data structures */
struct krping_stats {
	atomic_t send_msgs;
	atomic_t recv_msgs;
	atomic_t write_msgs;
	atomic_t read_msgs;
};
#endif

/*
 * Control block struct.
 */
struct krping_cb {
	int conn_no;
	int server;				/* 0: client */

	/* IB essentials */
	struct ib_cq *cq;		/* can split into two send/recv */
	struct ib_pd *pd;
	struct ib_qp *qp;
	struct ib_mr *dma_mr;

	struct ib_fast_reg_page_list *page_list;
	int page_list_len;

	/* Parameters */
	int server_invalidate;
	int recv_size;
	int read_inv;
	u8 key;

	/* Send buffer */
	struct ib_send_wr sq_wr;			/* send work requrest info for send */
	struct ib_sge send_sgl;
	struct pcn_kmsg_message send_buf;	/* single send buf - msg unit */
	u64 send_dma_addr;					/* for send buffer */
	u64 send_mapping;					/* for unmapping */

	/* RDMA common */
	struct ib_reg_wr reg_mr_wr_act;		/* can reg be placed with rdma (name)?TODO */
	struct ib_reg_wr reg_mr_wr_pass;
	struct ib_send_wr inv_wr_act;
	struct ib_send_wr inv_wr_pass;
	struct ib_mr *reg_mr_act;
	struct ib_mr *reg_mr_pass;

	struct ib_rdma_wr rdma_sq_wr;	/* rdma work request info for WR */
	struct ib_sge rdma_sgl;			/* rdma single SGE */

	uint32_t remote_rkey;		/* temporary saving remote RKEY */
	uint64_t remote_addr;		/* temporary saving remote TO */
	uint32_t remote_len;		/* temporary saving remote LEN */

	/* RDMA buf for active */
	u64 active_dma_addr;	 	/* for active buffer */
	u64 active_rdma_mapping;
	struct page *act_page;		/* active page */
	unsigned char *act_paddr;	/* active mapped addr for the page */
	struct ib_mr *rdma_mr;		/* active_mr */

	/* RDMA buf for passive */
	u64  passive_dma_addr;		/* passive R/W buffer addr */
	u64 passive_rdma_mapping;
	struct page *pass_page;		/* passive page */
	unsigned char *pass_paddr;  /* passive mapped addr for the page */
	struct ib_mr *start_mr;		/* passive_mr */

	/* Connection */
	uint16_t port;				/* dst port in NBO */
	u8 addr[16];				/* dst addr in NBO */
	const char *addr_str;		/* dst addr string */
	uint8_t addr_type;			/* ADDR_FAMILY - IPv4/V6 */
	int txdepth;				/* SQ depth */
	unsigned long rdma_size;	/* ping data size */
	
	/* CM stuff */
	struct rdma_cm_id *cm_id;		/* connection on client side */
									/* listener on server side */
	struct rdma_cm_id *child_cm_id;	/* connection on server side */

	/* Wait-event states */
	atomic_t state;				/* used for connection */
	atomic_t send_state;		/* for send completion */
	atomic_t read_state;
	atomic_t write_state;
	wait_queue_head_t sem;

	/* Synchronization */
	struct mutex send_mutex;
	struct mutex recv_mutex;
	struct mutex active_mutex;
	struct mutex passive_mutex;	/* passive lock*/
	struct mutex qp_mutex;		/* protect ib_post_send(qp) */

#ifdef CONFIG_POPCORN_DEBUG_MSG_LAYER_VERBOSE
	/* Dbging sync problem */
	atomic_t active_cnt;		/* used for cond/signalling */
	atomic_t passive_cnt;		/* used for cond/signalling */
	atomic_t g_all_ticket;
	struct krping_stats stats;
#endif
};


/*
 * List of running IB threads. 
 */
struct krping_cb *cb[MAX_NUM_NODES];
EXPORT_SYMBOL(cb);
struct krping_cb *cb_listen;

/* For RECV wc_struct queue */
spinlock_t wcs_q_mutex[MAX_NUM_NODES];
struct completion wcs_comp[MAX_NUM_NODES];
static struct wc_struct wcs_wait_q[MAX_NUM_CHANNELS];	/* Sentinel */

/* functions */
static void __krping_cq_event_handler(struct ib_cq *cq, void *ctx);

/* Popcorn utilities */
static int __init initialize(void);
extern pcn_kmsg_cbftn callbacks[PCN_KMSG_TYPE_MAX];
extern send_cbftn send_callback;
extern send_rdma_cbftn send_callback_rdma;
extern handle_rdma_request_ftn handle_rdma_callback;
extern char *msg_layer;

/* workqueue */
struct workqueue_struct *msg_handler;

/* workqueue arg */
typedef struct {
	struct work_struct work;
	struct pcn_kmsg_message *lmsg;
} pcn_kmsg_work_t;


/* Reuse recv wcs */
static void __enq_wcs(struct wc_struct *strc, int index)
{
	spin_lock(&wcs_q_mutex[index]);
    list_add_tail(&(strc->list), &(wcs_wait_q[index].list));
	//complete(&wcs_comp[index]);
	spin_unlock(&wcs_q_mutex[index]);
}

static struct wc_struct *__dq_wcs(int index)
{
	struct wc_struct *tmp;

	//wait_for_completion(&wcs_comp[index]);
	spin_lock(&wcs_q_mutex[index]);
    if (list_empty(&wcs_wait_q[index].list)) {
        printk(KERN_INFO "List %d is empty...\n", index);
		spin_unlock(&wcs_q_mutex[index]);
        return NULL;
    } else {
        tmp = list_first_entry(&wcs_wait_q[index].list,
								struct wc_struct, list);
        list_del(wcs_wait_q[index].list.next);
		spin_unlock(&wcs_q_mutex[index]);
        return tmp;
    }
}


static int __krping_cma_event_handler(struct rdma_cm_id *cma_id,
										struct rdma_cm_event *event)
{
	int ret;
	struct krping_cb *_cb = cma_id->context; /* use cm_id to retrive cb */
	static int cma_event_cnt = 0, conn_event_cnt = 0;
	KRPRINT_INIT("[[[[[external]]]]] conn_no %d (%s) >>>>>>>> %s(): "
			  "cma_event type %d cma_id %p (%s)\n", _cb->conn_no,
			(my_nid == _cb->conn_no) ? "server" : "client", __func__,
			event->event, cma_id, (cma_id == _cb->cm_id) ? "parent" : "child");
	KRPRINT_INIT("< cma_id %p _cb->cm_id %p >\n", cma_id, _cb->cm_id);

	switch (event->event) {
	case RDMA_CM_EVENT_ADDR_RESOLVED:
		KRPRINT_INIT("< ---------- RDMA_CM_EVENT_ADDR_RESOLVED --------- >\n");
		atomic_set(&_cb->state, ADDR_RESOLVED);
		ret = rdma_resolve_route(cma_id, 2000);
		if (ret) {
			printk(KERN_ERR "< rdma_resolve_route error %d >\n", ret);
			wake_up_interruptible(&_cb->sem);
		}
		break;

	case RDMA_CM_EVENT_ROUTE_RESOLVED:
		atomic_set(&_cb->state, ROUTE_RESOLVED);
		wake_up_interruptible(&_cb->sem);
		break;

	case RDMA_CM_EVENT_CONNECT_REQUEST:
		atomic_set(&_cb->state, CONNECT_REQUEST);
		KRPRINT_INIT("< ----CONNECT_REQUEST----: node %d _cb->child_cm_id %p = "
						"cma_id(external) >\n", _cb->conn_no, _cb->child_cm_id);
		/* distributed to other connections */
		_cb->child_cm_id = cma_id;
		KRPRINT_INIT("< ----CONNECT_REQUEST----: node %d _cb->child_cm_id %p = "
						"cma_id(external) >\n", _cb->conn_no, _cb->child_cm_id);
		wake_up_interruptible(&_cb->sem);
		break;

	case RDMA_CM_EVENT_ESTABLISHED:
		KRPRINT_INIT("< ---------- CONNECTION ESTABLISHED ------------- >\n");
		if (cb[my_nid]->conn_no == _cb->conn_no) {
			cma_event_cnt++;

			atomic_set(&cb[my_nid+cma_event_cnt]->state, CONNECTED);
			wake_up_interruptible(&cb[my_nid+cma_event_cnt]->sem);
			KRPRINT_INIT("< my business >: successful accept\n");
			KRPRINT_INIT("< cb[my_nid]->conn_no %d _cb->conn_no %d "
								"cma_event_cnt %d conn_event_cnt %d>\n",
										cb[my_nid]->conn_no, _cb->conn_no,
											cma_event_cnt, conn_event_cnt);
		} else {
			atomic_set(&cb[conn_event_cnt]->state, CONNECTED);
			KRPRINT_INIT("< none of my business >: successful connect\n");
			KRPRINT_INIT("< cb[my_nid]->conn_no %d _cb->conn_no %d "
								"cma_event_cnt %d conn_event_cnt %d>\n",
										cb[my_nid]->conn_no, _cb->conn_no,
											cma_event_cnt, conn_event_cnt);
			wake_up_interruptible(&cb[conn_event_cnt]->sem);
			conn_event_cnt++;
		}
		break;

	case RDMA_CM_EVENT_ADDR_ERROR:
	case RDMA_CM_EVENT_ROUTE_ERROR:
	case RDMA_CM_EVENT_CONNECT_ERROR:
	case RDMA_CM_EVENT_UNREACHABLE:
	case RDMA_CM_EVENT_REJECTED:
		printk(KERN_ERR "< cma event %d, error %d >\n", event->event,
														event->status);
		atomic_set(&_cb->state, ERROR);
		wake_up_interruptible(&_cb->sem);
		break;

	case RDMA_CM_EVENT_DISCONNECTED:
		printk(KERN_ERR "< -----DISCONNECT EVENT------... >\n");
		KRPRINT_INIT("< %s(): _cb->state = %d, CONNECTED=%d >\n",
						__func__, (int)atomic_read(&_cb->state), CONNECTED);
		atomic_set(&_cb->state, ERROR);
		wake_up_interruptible(&_cb->sem);
		break;

	case RDMA_CM_EVENT_DEVICE_REMOVAL:
		printk(KERN_ERR "< -----cma detected device removal!!!!----- >\n");
		break;

	default:
		printk(KERN_ERR "< -----oof bad type!----- >\n");
		wake_up_interruptible(&_cb->sem);
		break;
	}
	return 0;
}

/*
 * Create a recv_sql/rq_we
 */
struct ib_recv_wr* __create_recv_wr(int conn_no, bool is_int)
{
	struct krping_cb *_cb = cb[conn_no];
	struct pcn_kmsg_message *element_addr;
	struct ib_sge *_recv_sgl;
	struct ib_recv_wr *_rq_wr;
	struct wc_struct *wcs;
	u64 element_dma_addr;

	element_addr = kmalloc(sizeof(*element_addr), GFP_KERNEL);
	if (!element_addr) {
		printk(KERN_ERR "recv_buf malloc failed\n");
		BUG();
	}

	_recv_sgl = kmalloc(sizeof(*_recv_sgl), GFP_KERNEL);
	if (!_recv_sgl) {
		printk(KERN_ERR "sgl recv_buf malloc failed\n");
		BUG();
	}

	_rq_wr =  kmalloc(sizeof(*_rq_wr), GFP_KERNEL);
	if (!_rq_wr) {
		printk(KERN_ERR "rq_wr recv_buf malloc failed\n");
		BUG();
	}

	wcs = kmalloc(sizeof(*wcs), GFP_KERNEL);
	if (!wcs) {
		printk(KERN_ERR "wcs malloc failed\n");
		BUG();
	}

	/* map buf to ib addr space */
	element_dma_addr = dma_map_single(_cb->pd->device->dma_device,
						  element_addr, _cb->recv_size, DMA_BIDIRECTIONAL);

	/* set up sgl */
	_recv_sgl->length = _cb->recv_size;
	_recv_sgl->addr = element_dma_addr;
	_recv_sgl->lkey = _cb->pd->local_dma_lkey;

	/* set up rq_wr */
	_rq_wr->sg_list = _recv_sgl;
	_rq_wr->num_sge = 1;
	_rq_wr->wr_id = (u64)wcs;
	_rq_wr->next = NULL;

	/* save all address to release */
	wcs->element_addr = element_addr;
	wcs->recv_sgl = _recv_sgl;
	wcs->rq_wr = _rq_wr;
	INIT_LIST_HEAD(&wcs->list);

	//MSGDPRINTK("_rq_wr %p _cb->recv_size %d element_addr %p\n",
	//				(void*)_rq_wr, _cb->recv_size, (void*)element_addr);
	return _rq_wr;
}


static int __krping_connect_client(struct krping_cb *_cb)
{
	int ret;
	struct rdma_conn_param conn_param;

	KRPRINT_INIT("->%s();\n", __func__);

	memset(&conn_param, 0, sizeof conn_param);
	conn_param.responder_resources = g_conn_responder_resuorces;
	conn_param.initiator_depth = g_conn_initiator_depth;
	conn_param.retry_count = g_conn_retry_count;

	ret = rdma_connect(_cb->cm_id, &conn_param);
	if (ret) {
		printk(KERN_ERR "rdma_connect error %d\n", ret);
		return ret;
	}

	wait_event_interruptible(_cb->sem,
							atomic_read(&_cb->state) == CONNECTED);
	if (atomic_read(&_cb->state) == ERROR) {
		printk(KERN_ERR "wait for CONNECTED state %d\n",
								atomic_read(&_cb->state));
		return -1;
	}

	KRPRINT_INIT("rdma_connect successful\n");
	return 0;
}

static void __fill_sockaddr(struct sockaddr_storage *sin, struct krping_cb *_cb)
{
	memset(sin, 0, sizeof(*sin));

	if (!_cb->server) {
		/* client: load as usuall (ip=remote) */
		if (_cb->addr_type == AF_INET) {
			struct sockaddr_in *sin4 = (struct sockaddr_in *)sin;
			sin4->sin_family = AF_INET;
			memcpy((void *)&sin4->sin_addr.s_addr, _cb->addr, 4);
			sin4->sin_port = _cb->port;
		}
		KRPRINT_INIT("client IP fillup _cb->addr %s _cb->port %d\n",
												_cb->addr, _cb->port);
	} else {
		/* cb->server: load from global (ip=itself) */
		if (cb[my_nid]->addr_type == AF_INET) {
			struct sockaddr_in *sin4 = (struct sockaddr_in *)sin;
			sin4->sin_family = AF_INET;
			memcpy((void *)&sin4->sin_addr.s_addr, cb[my_nid]->addr, 4);
			sin4->sin_port = cb[my_nid]->port;
			KRPRINT_INIT("server IP fillup cb[my_nid]->addr %s"
										" cb[my_nid]->port %d\n",
										cb[my_nid]->addr, cb[my_nid]->port);
		}
	}
}

/*
 *	  IB/mlx5: Remove support for IB_DEVICE_LOCAL_DMA_LKEY (FASTREG)
 */
static int __reg_supported(struct ib_device *dev)
{
	int ret;
	struct ib_device_attr device_attr;
	u64 needed_flags = IB_DEVICE_MEM_MGT_EXTENSIONS |
						IB_DEVICE_LOCAL_DMA_LKEY;
	ret = ib_query_device(dev, &device_attr);

	MSGDPRINTK("%s(): IB_DEVICE_MEM_WINDOW %d support?%d\n",
				__func__, IB_DEVICE_MEM_WINDOW,
				device_attr.device_cap_flags&IB_DEVICE_MEM_WINDOW);
	MSGDPRINTK("%s(): IB_DEVICE_MEM_MGT_EXTENSIONS %d\n",
				__func__, IB_DEVICE_MEM_MGT_EXTENSIONS);
	MSGDPRINTK("%s(): IB_DEVICE_LOCAL_DMA_LKEY %d\n",
				__func__, IB_DEVICE_LOCAL_DMA_LKEY);
	MSGDPRINTK("%s(): (device_attr.device_cap_flags & needed_flags) %llx\n",
				__func__, (device_attr.device_cap_flags & needed_flags));

	if ((device_attr.device_cap_flags & needed_flags) != needed_flags) {
		printk(KERN_ERR "Fastreg not supported - device_cap_flags 0x%llx\n",
											(u64)device_attr.device_cap_flags);
		return 1;
	}
	MSGDPRINTK("Fastreg/local_dma_lkey supported - device_cap_flags 0x%llx\n",
											(u64)device_attr.device_cap_flags);
	return 1;
}

static int __krping_bind_server(struct krping_cb *cb)
{
	struct sockaddr_storage sin;
	int ret;

	__fill_sockaddr(&sin, cb);

	ret = rdma_bind_addr(cb->cm_id, (struct sockaddr *)&sin);
	if (ret) {
		printk(KERN_ERR "rdma_bind_addr error %d\n", ret);
		return ret;
	}

	ret = rdma_listen(cb->cm_id, LISTEN_BACKLOG);
	if (ret) {
		printk(KERN_ERR "rdma_listen failed: %d\n", ret);
		return ret;
	}

	return 0;
}

static void __setup_send_wr(struct krping_cb *cb,
							struct pcn_kmsg_message *lmsg)
{
	cb->send_dma_addr = dma_map_single(cb->pd->device->dma_device,
							lmsg, lmsg->header.size,
							DMA_BIDIRECTIONAL);
	pci_unmap_addr_set(cb, send_mapping, cb->send_dma_addr);

	/* Send buffer */
	cb->send_sgl.addr = cb->send_dma_addr;
	cb->send_sgl.length = lmsg->header.size;

	MSGPRINTK("@@@ <send addr (synamical mapping)>\n");
	MSGDPRINTK("@@@ lmsg = %p\n", (void*)lmsg);
	MSGDPRINTK("@@@ cb->send_sgl.addr = %p\n", (void*)cb->send_sgl.addr);
								// user vaddr (O) mapped to the next line
	//MSGDPRINTK("@@@ sizeof(cb->send_buf) = %ld\n", sizeof cb->send_buf);
	//MSGDPRINTK("@@@ sizeof(*lmsg) = %ld (X)\n", sizeof(*lmsg));
	//MSGDPRINTK("@@@ strlen(lmsg) = %ld (X)\n", strlen((char*)lmsg));
	MSGDPRINTK("@@@ lmsg->header.size = %d (O)\n", lmsg->header.size);
								// kernel addr (X) (mapped to each other)
	MSGDPRINTK("\n");
}

/* set up sgl, used for rdma */
static void __krping_setup_wr(struct krping_cb *cb)
{
	int i=0, ret;

	/* Pre-post RECV buffers */
	MSGPRINTK("\n\n\n->%s(): \n", __func__);
	MSGPRINTK("@@@ 2 cb->recv_size = %d\n", cb->recv_size);
	for(i=0;i<MAX_RECV_WR;i++) {
		struct ib_recv_wr *bad_wr;
		struct ib_recv_wr *_rq_wr = __create_recv_wr(cb->conn_no, false);

		if (i<5 || i>(MAX_RECV_WR-5)) {
			MSGPRINTK("_rq_wr %p cb->conn_no %d recv_size %d wr_id %p\n",
			(void*)_rq_wr, cb->conn_no, cb->recv_size, (void*)_rq_wr->wr_id);
		}

		/* 90% posted, 10% reserved */
		if (i <= MAX_RECV_WR*90/100) {
			ret = ib_post_recv(cb->qp, _rq_wr, &bad_wr);
			if (ret) {
				printk(KERN_ERR "ib_post_recv failed: %d\n", ret);
				BUG();
			}
		} else {
			__enq_wcs((struct wc_struct *)_rq_wr->wr_id, cb->conn_no);
		}
	}

	/* send buffer: unchanged parameters */
	//cb->send_sgl.lkey = cb->qp->device->local_dma_lkey; // A BUG from kprint.c
	//cb->send_sgl.lkey = cb->reg_mr_act->lkey;			// A BUG from kprint.c
	cb->send_sgl.lkey = cb->pd->local_dma_lkey;			// Fixed the BUG (O)
	MSGDPRINTK("@@@ <lkey>\n");
	MSGDPRINTK("@@@ lkey=%d from ../mad.c (ctx->pd->local_dma_lkey)\n",
							cb->pd->local_dma_lkey);
	MSGDPRINTK("@@@ cb->qp->device->local_dma_lkey = %d = 0\n",
				cb->qp->device->local_dma_lkey);
	MSGDPRINTK("@@@ lkey=%d from client/server example(cb->mr->lkey)\n",
							cb->reg_mr_act->lkey);

	/* normal send/recv */
	cb->sq_wr.opcode = IB_WR_SEND;
	cb->sq_wr.send_flags = IB_SEND_SIGNALED;
	/* sge */
	cb->sq_wr.sg_list = &cb->send_sgl;
	cb->sq_wr.num_sge = 1;

	/*
	send_flags: Extra attributes for the Send Request:
	- IB_SEND_FENCE: Before performing this operation,
					wait until the processing of prior Send Requests has ended.
	- IB_SEND_SIGNALED: If the QP was created with selective signaling,
						when the processing of this Send Request is ended,
						a Work Completion will be generated.
	- IB_SEND_SOLICITED: Mark that a Solicited event will be created
							in the remote side.
	- IB_SEND_INLINE: Post this Send Request as inline—that is,
						let the low-level driver read the memory buffers in
						if sg_list instead of the RDMA device; this may increase
						the latency.
	- IB_SEND_IP_CSUM: Send an IPoIB message and calculate the IP checksum in HW
						(checksum offload).
	*/

	/* READ/WRITE passive buf are allocated dynamically in other places
	 *		active: active_dma_addr; passive: passive_dma_addr
	 *		used for seting up rdma_sgl.addr
	 *		e.g. cb->rdma_sgl.addr = cb->passive_dma_addr;
	 */

	/* Common for RW - RW wr */
	cb->rdma_sq_wr.wr.sg_list = &cb->rdma_sgl;
	cb->rdma_sq_wr.wr.send_flags = IB_SEND_SIGNALED;
	cb->rdma_sq_wr.wr.num_sge = 1;

	/*
	 * A chain of 2 WRs, INVALDATE_MR + REG_MR.
	 * both unsignaled (no completion).  The client uses them to reregister
	 * the rdma buffers with a new key each iteration.
	 */
	cb->reg_mr_wr_act.wr.opcode = IB_WR_REG_MR;		/* (legacy:fastreg) */
	cb->reg_mr_wr_act.mr = cb->reg_mr_act;

	cb->reg_mr_wr_pass.wr.opcode = IB_WR_REG_MR;	/* (legacy:fastreg) */
	cb->reg_mr_wr_pass.mr = cb->reg_mr_pass;


	/*
	 * 1. invalidate Memory Window
	 * 2. then register this new key to mr
	 */
	cb->inv_wr_act.opcode = IB_WR_LOCAL_INV;
	cb->inv_wr_act.next = &cb->reg_mr_wr_act.wr;

	cb->inv_wr_pass.next = &cb->reg_mr_wr_pass.wr;
	cb->inv_wr_pass.opcode = IB_WR_LOCAL_INV;
	/*  The reg mem_mode uses a reg mr on the client side for the (We are)
	 *  rw_passive_buf and rw_active_buf buffers.  Each time the client will 
	 *  advertise one of these buffers, it invalidates the previous registration 
	 *  and fast registers the new buffer with a new key.
	 *
	 *  If the server_invalidate	(We are not)
	 *  option is on, then the server will do the invalidation via the
	 * "go ahead" messages using the IB_WR_SEND_WITH_INV opcode. Otherwise the
	 * client invalidates the mr using the IB_WR_LOCAL_INV work request.
	 */

	return;
}

static int __krping_create_qp(struct krping_cb *cb)
{
	int ret;
	struct ib_qp_init_attr init_attr;

	memset(&init_attr, 0, sizeof(init_attr));

	/* send and recv queue depth */
	init_attr.cap.max_send_wr = cb->txdepth;
	init_attr.cap.max_recv_wr = MAX_RECV_WR*2;

	/* For flush_qp() */
	init_attr.cap.max_send_wr++;
	init_attr.cap.max_recv_wr++;

	init_attr.cap.max_recv_sge = 1;
	init_attr.cap.max_send_sge = 1;
	init_attr.qp_type = IB_QPT_RC;

	/* send and recv use a same cq */
	init_attr.send_cq = cb->cq;
	init_attr.recv_cq = cb->cq;
	init_attr.sq_sig_type = IB_SIGNAL_REQ_WR;

	/*	The IB_SIGNAL_REQ_WR flag means that not all send requests posted to
	 *	the send queue will generate a completion -- only those marked with
	 *	the IB_SEND_SIGNALED flag.  However, the driver can't free a send
	 *	request from the send queue until it knows it has completed, and the
	 *	only way for the driver to know that is to see a completion for the
	 *	given request or a later request.  Requests on a queue always complete
	 *	in order, so if a later request completes and generates a completion,
	 *	the driver can also free any earlier unsignaled requests)
	 */

	if (cb->server) {
		ret = rdma_create_qp(cb->child_cm_id, cb->pd, &init_attr);
		if (!ret)
			cb->qp = cb->child_cm_id->qp;
	} else {
		ret = rdma_create_qp(cb->cm_id, cb->pd, &init_attr);
		if (!ret)
			cb->qp = cb->cm_id->qp;
	}

	return ret;
}

static int __krping_setup_qp(struct krping_cb *cb, struct rdma_cm_id *cm_id)
{
	int ret;
	struct ib_cq_init_attr attr = {0};

	MSGPRINTK("\n->%s();\n", __func__);

	//cb->pd = ib_alloc_pd(cm_id->device, 0);
	cb->pd = ib_alloc_pd(cm_id->device);
	if (IS_ERR(cb->pd)) {
		printk(KERN_ERR "ib_alloc_pd failed\n");
		return PTR_ERR(cb->pd);
	}
	MSGPRINTK("created pd %p\n", cb->pd);

	attr.cqe = cb->txdepth * SEND_DEPTH;
	attr.comp_vector = INT_MASK;
	cb->cq = ib_create_cq(cm_id->device,
							__krping_cq_event_handler, NULL, cb, &attr);
	if (IS_ERR(cb->cq)) {
		printk(KERN_ERR "ib_create_cq failed\n");
		ret = PTR_ERR(cb->cq);
		goto err1;
	}
	MSGPRINTK("created cq %p task\n", cb->cq);

	/* INT flag/mask raised */
	ret = ib_req_notify_cq(cb->cq, IB_CQ_NEXT_COMP);
	if (ret) {
		printk(KERN_ERR "ib_create_cq failed\n");
		goto err2;
	}

	ret = __krping_create_qp(cb);
	if (ret) {
		printk(KERN_ERR "krping_create_qp failed: %d\n", ret);
		goto err2;
	}
	MSGPRINTK("created qp %p\n", cb->qp);
	return 0;
err2:
	ib_destroy_cq(cb->cq);
err1:
	ib_dealloc_pd(cb->pd);
	return ret;
}

// init all buffers < 1.pd->cq->qp 2.[mr] 3.xxx >
static int __krping_setup_buffers(struct krping_cb *cb)
{
	int ret;
	MSGPRINTK("\n->%s();\n", __func__);
	MSGPRINTK("__krping_setup_buffers called on cb %p\n", cb);

	/* No send READ WRITE buffer is allocated statically */

	/* recv wq has been changed to be dinamically allocated */
	
	cb->page_list_len = (((cb->rdma_size - 1) & PAGE_MASK) + PAGE_SIZE)
															>> PAGE_SHIFT;

	KRPRINT_INIT("cb->rdma_size %lu, /PAGESIZE, cb->page_list_len %d \n",
										cb->rdma_size, cb->page_list_len);
	/* mr for active */
	cb->reg_mr_act = ib_alloc_mr(cb->pd, IB_MR_TYPE_MEM_REG, cb->page_list_len);
													// fill out lkey and rkey
	/* mr for passive */
	cb->reg_mr_pass = ib_alloc_mr(cb->pd, IB_MR_TYPE_MEM_REG,
														 cb->page_list_len);

	if (IS_ERR(cb->reg_mr_act)) {
		ret = PTR_ERR(cb->reg_mr_act);
		MSGPRINTK("reg_mr_act failed %d\n", ret);
		goto bail;
	}
	if (IS_ERR(cb->reg_mr_pass)) {
		ret = PTR_ERR(cb->reg_mr_pass);
		MSGPRINTK("reg_mr_pass failed %d\n", ret);
		goto bail;
	}

	MSGDPRINTK("\n@@@ after mr\n");
	MSGDPRINTK("@@@ reg rkey %d page_list_len %u\n",
										cb->reg_mr_act->rkey, cb->page_list_len);
	MSGDPRINTK("@@@ 1 cb->reg_mr_act->lkey %d from mr \n", cb->reg_mr_act->lkey);
	MSGDPRINTK("@@@ 1 correct lkey=%d (ref: ./drivers/infiniband/core/mad.c )"
				"(ctx->pd->local_dma_lkey)\n", cb->pd->local_dma_lkey);
																//4xxx dynamic
	__krping_setup_wr(cb);
	MSGPRINTK("allocated & registered buffers done!\n");
	MSGPRINTK("\n\n");
	return 0;
bail:
	if (cb->reg_mr_act && !IS_ERR(cb->reg_mr_act))
		ib_dereg_mr(cb->reg_mr_act);
	if (cb->reg_mr_pass && !IS_ERR(cb->reg_mr_pass))
		ib_dereg_mr(cb->reg_mr_pass);
	if (cb->rdma_mr && !IS_ERR(cb->rdma_mr))
		ib_dereg_mr(cb->rdma_mr);
	if (cb->dma_mr && !IS_ERR(cb->dma_mr))
		ib_dereg_mr(cb->dma_mr);
	return ret;
}


static int __krping_accept(struct krping_cb *_cb)
{
	int ret;
	struct rdma_conn_param conn_param;
	KRPRINT_INIT("->%s(); _cb->conn_%d accepting a connection request...\n",
														__func__, _cb->conn_no);
	memset(&conn_param, 0, sizeof conn_param);
	conn_param.responder_resources = 1;
	conn_param.initiator_depth = 1;

	ret = rdma_accept(_cb->child_cm_id, &conn_param);
	if (ret) {
		printk(KERN_ERR "rdma_accept error: %d\n", ret);
		return ret;
	}

	KRPRINT_INIT("->%s(): blocking wating for a signal %d..........\n",
											__func__, _cb->conn_no);
	wait_event_interruptible(_cb->sem,
								atomic_read(&_cb->state) == CONNECTED);
													// have a look child_cm_id
	if (atomic_read(&_cb->state) == ERROR) {
		printk(KERN_ERR "wait for CONNECTED state %d\n",
								atomic_read(&_cb->state));
		return -1;
	}
	KRPRINT_INIT("->%s(): got the signal !!!!(GOOD)!!!!!!! _cb->state = %d \n",
										__func__, atomic_read(&_cb->state));

	set_popcorn_node_online(_cb->conn_no, true);
	return 0;
}

static void __krping_free_buffers(struct krping_cb *cb)
{
	MSGPRINTK("%s(): called on cb %p\n", __func__, cb);

	if (cb->dma_mr)
		ib_dereg_mr(cb->dma_mr);
	if (cb->rdma_mr)
		ib_dereg_mr(cb->rdma_mr);
	if (cb->start_mr)
		ib_dereg_mr(cb->start_mr);
	if (cb->reg_mr_act)
		ib_dereg_mr(cb->reg_mr_act);
	if (cb->reg_mr_pass)
		ib_dereg_mr(cb->reg_mr_pass);
}

static void __krping_free_qp(struct krping_cb *cb)
{
	ib_destroy_qp(cb->qp);
	ib_destroy_cq(cb->cq);
	ib_dealloc_pd(cb->pd);
}

#if DEMON_POST_RECV_WCS
static int __recv_post(void* arg0)
{
	struct krping_cb *_cb = arg0;
	struct ib_recv_wr *bad_wr;
	int ret;

	while (1) {
		struct wc_struct *_wcs = __dq_wcs(_cb->conn_no);
		if(unlikely(!_wcs))
			break;

		ret = ib_post_recv(_cb->qp, _wcs->rq_wr, &bad_wr);

		if (ret) {
			printk(KERN_ERR "ib_post_recv failed: %d\n", ret);
			BUG();
		}
	}
	return 0;
}
#endif

static int __krping_persistent_server_thread(void* arg0)
{
	struct krping_cb *_cb = arg0;
	int ret = -1;

	KRPRINT_INIT("%s(): conn %d\n", __func__, _cb->conn_no);
	ret = __krping_setup_qp(_cb, _cb->child_cm_id);
	if (ret) {
		printk(KERN_ERR "setup_qp failed: %d\n", ret);
		goto err0;
	}

	ret = __krping_setup_buffers(_cb);
	if (ret) {
		printk(KERN_ERR "__krping_setup_buffers failed: %d\n", ret);
		goto err1;
	}
	/* after here, you can send/recv */

	ret = __krping_accept(_cb);
	if (ret) {
		printk(KERN_ERR "connect error %d\n", ret);
		goto err2;
	}
	return 0;
err2:
	__krping_free_buffers(_cb);
err1:
	__krping_free_qp(_cb);
err0:
	rdma_destroy_id(_cb->child_cm_id);
	return ret;
}

static int __krping_run_server(void* arg0)
{
	struct krping_cb *my_cb = arg0;
	int ret, i = 0;

	ret = __krping_bind_server(my_cb);
	if (ret)
		return ret;

	KRPRINT_INIT("\n\n\n");

	/* create multiple connections */
	while (1){
		struct krping_cb *target_cb;
		i++;

		KRPRINT_INIT("my_nid %d + i %d\n", my_nid, i);
		if (my_nid+i >= MAX_NUM_NODES) {
			KRPRINT_INIT("All connections are done\n");
			break;
		}

		/* Wait for client's Start STAG/TO/Len */
		wait_event_interruptible(my_cb->sem,
					atomic_read(&my_cb->state) == CONNECT_REQUEST);
		if (atomic_read(&my_cb->state) != CONNECT_REQUEST) {
			printk(KERN_ERR "wait for CONNECT_REQUEST state %d\n",
										atomic_read(&my_cb->state));
			continue;
		}
		atomic_set(&my_cb->state, IDLE);

		KRPRINT_INIT("Got a connection\n");

		target_cb = cb[my_nid+i];
		target_cb->server = 1;

		/* got from INT. Will be used [setup_qp(SRWRirq)] -> setup_buf -> */
		target_cb->child_cm_id = my_cb->child_cm_id;

		if (__krping_persistent_server_thread(target_cb))
			rdma_disconnect(target_cb->child_cm_id);

		printk("conn_no %d is ready (GOOD)\n", target_cb->conn_no);
	}
	return 0;
}

static int __krping_bind_client(struct krping_cb *cb)
{
	struct sockaddr_storage sin;
	int ret;

	__fill_sockaddr(&sin, cb);

	ret = rdma_resolve_addr(cb->cm_id, NULL, (struct sockaddr *)&sin, 2000);
	if (ret) {
		printk(KERN_ERR "rdma_resolve_addr error %d\n", ret);
		return ret;
	}

	wait_event_interruptible(cb->sem,
								atomic_read(&cb->state) == ROUTE_RESOLVED);
	if (atomic_read(&cb->state) != ROUTE_RESOLVED) {
		printk(KERN_ERR "addr/route resolution did not resolve: state %d\n",
													atomic_read(&cb->state));
		return -EINTR;
	}

	if (!__reg_supported(cb->cm_id->device))
		return -EINVAL;

	MSGPRINTK("rdma_resolve_addr - rdma_resolve_route successful\n");
	return 0;
}

static int __krping_run_client(struct krping_cb *cb)
{
	int ret;

	KRPRINT_INIT("====================================\n");
	KRPRINT_INIT("<<<<<<<< %s(): cb->conno %d >>>>>>>>\n", __func__, cb->conn_no);
	KRPRINT_INIT("====================================\n");
	ret = __krping_bind_client(cb);
	if (ret)
		return ret;

	ret = __krping_setup_qp(cb, cb->cm_id);
	if (ret) {
		printk(KERN_ERR "setup_qp failed: %d\n", ret);
		return ret;
	}

	ret = __krping_setup_buffers(cb);
	if (ret) {
		printk(KERN_ERR "__krping_setup_buffers failed: %d\n", ret);
		goto err1;
	}

	ret = __krping_connect_client(cb);
	if (ret) {
		printk(KERN_ERR "connect error %d\n", ret);
		goto err2;
	}
	return 0;
err2:
	__krping_free_buffers(cb);
err1:
	__krping_free_qp(cb);
	return ret;
}


/*
 * IB utility functions for supporting dynamic mapping
 * specifically for RW users such as page migration
 */
u64 __ib_map_act(void *paddr, int conn_no, int rw_size)
{
	struct krping_cb *_cb = cb[conn_no];
	u64 dma_addr = dma_map_single(_cb->pd->device->dma_device,
							  paddr, rw_size, DMA_BIDIRECTIONAL);
	pci_unmap_addr_set(_cb, active_rdma_mapping, dma_addr);
	return dma_addr;
}

u64 __ib_map_pass(void *paddr, int conn_no, int rw_size)
{
	struct krping_cb *_cb = cb[conn_no];
	u64 dma_addr = dma_map_single(_cb->pd->device->dma_device,
							  paddr, rw_size, DMA_BIDIRECTIONAL);
	pci_unmap_addr_set(_cb, passive_rdma_mapping, dma_addr);
	return dma_addr;
}

/* no need to umap, just remap directly */
void __unmap_act(int conn_no, int rw_size)
{
	struct krping_cb *_cb = cb[conn_no];

	DEBUG_LOG_V("act: unmap\n");
	dma_unmap_single(_cb->pd->device->dma_device,
						pci_unmap_addr(_cb, active_rdma_mapping),
										rw_size, DMA_BIDIRECTIONAL);
}

void __unmap_pass(int conn_no, int rw_size)
{
	struct krping_cb *_cb = cb[conn_no];

	DEBUG_LOG_V("pass: unmap pass buf\n");
	dma_unmap_single(_cb->pd->device->dma_device,
						pci_unmap_addr(_cb, passive_rdma_mapping),
										rw_size, DMA_BIDIRECTIONAL);
}


/*
 * Return the (possibly rebound) rkey for the rdma buffer.
 * REG mode: invalidate and rebind via reg wr.
 * Other modes: just return the mr rkey.
 */
u32 __krping_rdma_rkey_act(struct krping_cb *_cb, u64 buf, int post_inv, int rdma_len)
{
	u32 rkey;
	struct ib_send_wr *bad_wr;
	int ret;
	struct scatterlist sg = {0};

	/* old key - save corrent reg rkey (if dynamic) */
	_cb->inv_wr_act.ex.invalidate_rkey = _cb->reg_mr_act->rkey;

	/* Update the reg key - keeps the key the same */
	ib_update_fast_reg_key(_cb->reg_mr_act, _cb->key);
	_cb->reg_mr_wr_act.key = _cb->reg_mr_act->rkey;

	/* Setup permissions, reg_mr_wr_pass is in another function */
	/*	In krping.c
	 *	local about to perform READ: IB_ACCESS_REMOTE_READ
	 *
	 *	local about to perform WRITE: 	IB_ACCESS_LOCAL_WRITE |
	 *									IB_ACCESS_REMOTE_WRITE
	 */

	_cb->reg_mr_wr_act.access = IB_ACCESS_REMOTE_READ	|
								IB_ACCESS_REMOTE_WRITE	|
								IB_ACCESS_LOCAL_WRITE	|
								IB_ACCESS_REMOTE_ATOMIC;

	sg_dma_address(&sg) = buf;
	sg_dma_len(&sg) = rdma_len;
	DEBUG_LOG("%s(): rdma_len (dynamical) %d\n", __func__, sg_dma_len(&sg));

	ret = ib_map_mr_sg(_cb->reg_mr_act, &sg, 1, PAGE_SIZE);
										// snyc ib_dma_sync_single_for_cpu/dev
	BUG_ON(ret <= 0 || ret > _cb->page_list_len);

	DEBUG_LOG("%s(): ### post_inv = %d, reg_mr_act new rkey %d pgsz %u len %u"
			" rdma_len (dynamical) %d iova_start %llx\n", __func__, post_inv,
			_cb->reg_mr_wr_act.key, _cb->reg_mr_act->page_size, _cb->reg_mr_act->length,
			rdma_len, _cb->reg_mr_act->iova);
	/*
	 * ib_map_mr_sg() - Map the largest prefix of a dma mapped SG list
	 *     and set it the memory region.
	 * @mr:            memory region
	 * @sg:            dma mapped scatterlist
	 * @sg_nents:      number of entries in sg
	 * @sg_offset:     offset in bytes into sg
	 * @page_size:     page vector desired page size
	 *
	 * Constraints:
	 * - The first sg element is allowed to have an offset.
	 * - Each sg element must be aligned to page_size (or physically
	 *   contiguous to the previous element). In case an sg element has a
	 *   non contiguous offset, the mapping prefix will not include it.
	 * - The last sg element is allowed to have length less than page_size.
	 * - If sg_nents total byte length exceeds the mr max_num_sge * page_size
	 *   then only max_num_sg entries will be mapped.
	 * - If the MR was allocated with type IB_MR_TYPE_SG_GAPS_REG, non of these
	 *   constraints holds and the page_size argument is ignored.
	 *
	 * Returns the number of sg elements that were mapped to the memory region.
	 *
	 * After this completes successfully, the  memory region
	 * is ready for registration.
	 */

	mutex_lock(&_cb->qp_mutex);
	if (likely(post_inv))
		/* no inv from remote, so manually does it in local side */
		ret = ib_post_send(_cb->qp, &_cb->inv_wr_act, &bad_wr);	// INV+MR
	else
		ret = ib_post_send(_cb->qp, &_cb->reg_mr_wr_act.wr, &bad_wr);// MR
	mutex_unlock(&_cb->qp_mutex);

	if (ret) {
		printk(KERN_ERR "post send error %d\n", ret);
		atomic_set(&_cb->state, ERROR);
		atomic_set(&_cb->send_state, ERROR);
		atomic_set(&_cb->read_state, ERROR);
		atomic_set(&_cb->write_state, ERROR);
	}

	rkey = _cb->reg_mr_act->rkey;
	return rkey;
}

u32 __krping_rdma_rkey_pass(struct krping_cb *_cb,
							 u64 buf, int post_inv, int rdma_len)
{
	u32 rkey;
	struct ib_send_wr *bad_wr;
	int ret;
	struct scatterlist sg = {0};


	/* key generated by local and used for remote - going to be invalidated*/
	_cb->inv_wr_pass.ex.invalidate_rkey = _cb->reg_mr_pass->rkey;

	/* update new key */
	ib_update_fast_reg_key(_cb->reg_mr_pass, _cb->key);
	_cb->reg_mr_wr_pass.key = _cb->reg_mr_pass->rkey;

	/*	In krping.c
	 *	local going to perform READ: IB_ACCESS_REMOTE_READ
	 *
	 *	local going to perform WRITE: 	IB_ACCESS_LOCAL_WRITE |
	 *									IB_ACCESS_REMOTE_WRITE
	 */
	_cb->reg_mr_wr_pass.access = IB_ACCESS_REMOTE_READ	|
								IB_ACCESS_REMOTE_WRITE	|
								IB_ACCESS_LOCAL_WRITE	|
								IB_ACCESS_REMOTE_ATOMIC;

	sg_dma_address(&sg) = buf;
	sg_dma_len(&sg) = rdma_len;

	ret = ib_map_mr_sg(_cb->reg_mr_pass, &sg, 1, PAGE_SIZE);
	BUG_ON(ret <= 0 || ret > _cb->page_list_len);

	/**
	 * ib_map_mr_sg() - Map the largest prefix of a dma mapped SG list
	 *     and set it the memory region.
	 * @mr:            memory region
	 * @sg:            dma mapped scatterlist
	 * @sg_nents:      number of entries in sg
	 * @sg_offset:     offset in bytes into sg
	 * @page_size:     page vector desired page size
	 *
	 * Constraints:
	 * - The first sg element is allowed to have an offset.
	 * - Each sg element must be aligned to page_size (or physically
	 *   contiguous to the previous element). In case an sg element has a
	 *   non contiguous offset, the mapping prefix will not include it.
	 * - The last sg element is allowed to have length less than page_size.
	 * - If sg_nents total byte length exceeds the mr max_num_sge * page_size
	 *   then only max_num_sg entries will be mapped.
	 * - If the MR was allocated with type IB_MR_TYPE_SG_GAPS_REG, non of these
	 *   constraints holds and the page_size argument is ignored.
	 *
	 * Returns the number of sg elements that were mapped to the memory region.
	 *
	 * After this completes successfully, the  memory region
	 * is ready for registration.
	 */

	MSG_RDMA_PRK("%s(): ### post_inv = %d, reg_mr_wr_pass new rkey %d "
				 "pgsz %u len %u rdma_len (dynamical) %d iova_start %llx\n",
				 __func__, post_inv, _cb->reg_mr_wr_pass.key,
				 _cb->reg_mr_pass->page_size, _cb->reg_mr_pass->length,
										rdma_len, _cb->reg_mr_pass->iova);

	mutex_lock(&_cb->qp_mutex);
	/* in krping.c, READ: likely, WRITE: unlikely. BUT NOT HERE */
	if (likely(post_inv))
		/* no inv from remote, so manually does it in local side */
		ret = ib_post_send(_cb->qp, &_cb->inv_wr_pass, &bad_wr);
	else
		ret = ib_post_send(_cb->qp, &_cb->reg_mr_wr_pass.wr, &bad_wr);
	mutex_unlock(&_cb->qp_mutex);

	if (ret) {
		printk(KERN_ERR "post send error %d\n", ret);
		atomic_set(&_cb->state, ERROR);
		atomic_set(&_cb->send_state, ERROR);
		atomic_set(&_cb->read_state, ERROR);
		atomic_set(&_cb->write_state, ERROR);
	}

	rkey = _cb->reg_mr_pass->rkey;
	return rkey;
}


/*
 * User doesn't have to take care of concurrency problems.
 * This func will take care of it.
 * User has to free the allocated mem manually
 */
int __ib_kmsg_send_long(unsigned int dest_cpu,
					  struct pcn_kmsg_message *lmsg,
					  unsigned int msg_size)
{
	int ret;
	struct ib_send_wr *bad_wr;

	if ( msg_size > sizeof(struct pcn_kmsg_message)) {
		printk("%s(): ERROR - MSG %d larger than MAX_MSG_SIZE %ld\n",
					__func__, msg_size, sizeof(struct pcn_kmsg_message));
		BUG();
	}

	if (dest_cpu == my_nid) {
		printk(KERN_ERR "No support for sending msg to itself %d\n", dest_cpu);
		return 0;
	}

	lmsg->header.size = msg_size;
	lmsg->header.from_nid = my_nid;

	// pcn_msg (abstraction msg layer)
	//----------------------------------------------------------
	// ib

	MSG_SYNC_PRK("////////////////lock() conn %d/////////////\n", dest_cpu);
	mutex_lock(&cb[dest_cpu]->send_mutex);

#ifdef CONFIG_POPCORN_DEBUG_MSG_LAYER_VERBOSE
	lmsg->header.ticket = atomic_inc_return(&g_send_ticket);
	MSGPRINTK("%s(): send ticket %lu\n", __func__, lmsg->header.ticket);
#endif

	__setup_send_wr(cb[dest_cpu], lmsg);

	mutex_lock(&cb[dest_cpu]->qp_mutex);
	ret = ib_post_send(cb[dest_cpu]->qp, &cb[dest_cpu]->sq_wr, &bad_wr);
	mutex_unlock(&cb[dest_cpu]->qp_mutex);

	wait_event_interruptible(cb[dest_cpu]->sem,
			atomic_read(&cb[dest_cpu]->send_state) == RDMA_SEND_COMPLETE);
	atomic_set(&cb[dest_cpu]->send_state, IDLE);

	// unmap
	dma_unmap_single(cb[dest_cpu]->pd->device->dma_device,
					 pci_unmap_addr(cb[dest_cpu], send_mapping),
					 sizeof(cb[dest_cpu]->send_buf), DMA_BIDIRECTIONAL);

	mutex_unlock(&cb[dest_cpu]->send_mutex);
	MSG_SYNC_PRK("//////////////unlock() conn %d///////////////\n", dest_cpu);
	MSGDPRINTK("1 msg sent to dest_cpu %d!!!!!!\n\n", dest_cpu);
	return 0;
}

static void __handle_remote_thread_rdma_read_request(
						remote_thread_rdma_rw_t* inc_lmsg, void* target_paddr)
{
	remote_thread_rdma_rw_t* request = 
								(remote_thread_rdma_rw_t*) inc_lmsg;
	remote_thread_rdma_rw_t *reply; 
	struct ib_send_wr *bad_wr; // for ib_post_send
	struct krping_cb *_cb = cb[request->header.from_nid];
	int ret;

#ifdef CONFIG_POPCORN_DEBUG_MSG_LAYER_VERBOSE
	volatile unsigned long ts_start, ts_compose, ts_post, ts_end;
	MSGPRINTK("%s(): <<<<< passive READ request: "
				"my_nid=%d from_nid=%d rw_t %d recv_ticket %lu "
				"r_rdma_ticket %d msg_layer(good) >>>>>\n",
						__func__, my_nid, request->header.from_nid,
											request->rw_ticket,
											request->header.ticket,
											request->rdma_ticket);
#endif

	MSGDPRINTK("RPC passive READ request\n");

	/* send        ----->   irq (recv)
	 *                      [lock R]
	 *             =====>   perform READ
	 *                      unlock R
	 * irq (recv)  <-----   send
	 */

	mutex_lock(&_cb->passive_mutex);

#ifdef CONFIG_POPCORN_DEBUG_MSG_LAYER_VERBOSE
	atomic_inc(&_cb->passive_cnt);
	MSG_SYNC_PRK("////// READ passive lock() %d (active) rw_t %d ////////\n",
										(int)atomic_read(&_cb->passive_cnt),
														request->rw_ticket);
#endif

	_cb->passive_dma_addr = __ib_map_pass(target_paddr, _cb->conn_no,
												request->rdma_header.rw_size);

	/* perform READ (passive side) */
	/* performance evaluation */
	//- <time1 : compose msg info> -//
#ifdef CONFIG_POPCORN_DEBUG_MSG_LAYER_VERBOSE
	rdtscll(ts_start);
#endif

	/* RDMA READ echo data */
	// remote info:
	_cb->remote_rkey = ntohl(request->rdma_header.remote_rkey);	// redaundant
	_cb->remote_addr = ntohll(request->rdma_header.remote_addr);// redaundant
	_cb->remote_len = request->rdma_header.rw_size;				// redaundant

	_cb->rdma_sq_wr.rkey = _cb->remote_rkey;		// updated from remote!!!!
	_cb->rdma_sq_wr.remote_addr = _cb->remote_addr;	// updated from remote!!!!
	//_cb->rdma_sq_wr.wr.sg_list->length = _cb->remote_len;

	CHECK_LOG("<<<<< READ request: my_nid %d from_nid %d "
					"remote_rkey %d remote_addr %p rw_size %d>>>>>\n",
											my_nid, request->header.from_nid,
											_cb->remote_rkey,
											(void*)_cb->remote_addr,
											_cb->remote_len);

	// local info:
	//_active_dma_addr -> passive_dma_addr
	// register local buf for performing R/W (rdma_rkey)
	// rdma_sq_wr.wr.sg_list = &cb->rdma_sgl
	_cb->rdma_sgl.length = _cb->remote_len;
	_cb->rdma_sgl.addr = _cb->passive_dma_addr;
	_cb->rdma_sgl.lkey = __krping_rdma_rkey_pass(_cb, _cb->passive_dma_addr,
											!_cb->read_inv, _cb->remote_len);

	_cb->rdma_sq_wr.wr.next = NULL; // one work request

	/* Issue RDMA READ */
	if (unlikely(_cb->read_inv))
		_cb->rdma_sq_wr.wr.opcode = IB_WR_RDMA_READ_WITH_INV;
	else {
		/* Compose a READ sge with a invalidation */
		_cb->rdma_sq_wr.wr.opcode = IB_WR_RDMA_READ;

		/*	Jack: reserve this. just in case.
		 * 		In krping.c READ, they do send this redaundant FENCE.
		 * 		But it works find even if I take this part off.
		 *
		 * 		In krping.c WRITE, they do not send this redaundant FENCE.
		 *
		 *		In krping.c, they do READ and then WRITE and again and again.
		 */

		/*	To put a fence between an RDMA READ and the following SEND.
		 *
		 *	IB_SEND_FENCE: Before performing this operation, wait until
		 *	the processing of prior Send Requests has ended.
		 */

		/*
		// By originial example code
		struct ib_send_wr inv;
		// - Immediately follow the read with a fenced LOCAL_INV. - //
		_cb->rdma_sq_wr.wr.next = &inv;			// followed by a inv
		memset(&inv, 0, sizeof inv);
		inv.opcode = IB_WR_LOCAL_INV;
		inv.ex.invalidate_rkey = _cb->reg_mr_pass->rkey;
		inv.send_flags = IB_SEND_FENCE;
		*/
	}

	MSG_RDMA_PRK("ib_post_send R>>>>\n");
	//- <time 2: send> -//
#ifdef CONFIG_POPCORN_DEBUG_MSG_LAYER_VERBOSE
	rdtscll(ts_compose);
#endif

	mutex_lock(&_cb->qp_mutex);
	ret = ib_post_send(_cb->qp, &_cb->rdma_sq_wr.wr, &bad_wr);
	mutex_unlock(&_cb->qp_mutex);
	if (ret) {
		printk(KERN_ERR "post send error %d\n", ret);
		return;
	}
	/*	if just sent a FENCE, this should be turned on */
	//	_cb->rdma_sq_wr.wr.next = NULL;

	//- <time 3: send request done> /-/
#ifdef CONFIG_POPCORN_DEBUG_MSG_LAYER_VERBOSE
	rdtscll(ts_post);
#endif

	/* Wait for read completion */
	wait_event_interruptible(_cb->sem,
				(int)atomic_read(&_cb->read_state) == RDMA_READ_COMPLETE);
	/* passive READ done */
	atomic_set(&_cb->read_state, IDLE);

	//- <time 4: READ(task) done> /-/
#ifdef CONFIG_POPCORN_DEBUG_MSG_LAYER_VERBOSE
	rdtscll(ts_end);
	//- time result -//
	DEBUG_LOG("R: %d K compose_time %lu post_time %lu "
										"end_time %lu (cpu ticks)\n",
										(request->rdma_header.rw_size+1)/1024,
										ts_compose-ts_start, // +1 end char
										ts_post-ts_start, ts_end-ts_start);
	MSG_SYNC_PRK("/////// READ passive unlock() %d (active) rw_t %d ///////\n",
										(int)atomic_read(&_cb->passive_cnt),
										request->rw_ticket);
#endif

	CHECK_LOG("<<<<< CHECK rpc (passive) R_READ DONE size %d done\n"
						"_cb->rw_pass_buf(first10) \"%.10s\"\n"
						"_cb->rw_pass_buf(last 10) \"%.10s\"\n"
						"check READ here, not response\n\n\n",
						request->rdma_header.rw_size,
						dummy_pass_buf,
						request->rdma_header.rw_size>10?
						dummy_pass_buf+(request->rdma_header.rw_size-11):
						dummy_pass_buf);

	//__unmap_pass(_cb->conn_no, request->rdma_header.rw_size);

	/* send ----->   irq
	 *              lock R
	 *      =====>  perform READ
	 *              [unlock R]
	 * irq  <-----  send
	 *
	 */

	mutex_unlock(&_cb->passive_mutex);

	MSG_RDMA_PRK("%s(): send READ COMPLETION ACK !!! -->>\n", __func__);
	reply = pcn_kmsg_alloc_msg(sizeof(*reply));
	if (!reply)
		BUG();

	reply->header.type = request->rdma_header.rmda_type_res;
	reply->header.prio = PCN_KMSG_PRIO_NORMAL;
	//reply->tgroup_home_cpu = tgroup_home_cpu;
	//reply->tgroup_home_id = tgroup_home_id;

	/* RDMA R/W complete ACK */
	reply->header.is_rdma = true;
	reply->rdma_header.rdma_ack = true;
	reply->rdma_header.is_write = false;
	reply->rdma_header.remote_rkey = _cb->remote_rkey;
	reply->rdma_header.remote_addr = _cb->remote_addr;
	reply->rdma_header.rw_size = _cb->remote_len;

	/* for wait station */
	reply->remote_ws = inc_lmsg->remote_ws;

#ifdef CONFIG_POPCORN_DEBUG_MSG_LAYER_VERBOSE
	reply->rdma_ticket = request->rdma_ticket;
	reply->rw_ticket = request->rw_ticket;
#endif

	__ib_kmsg_send_long(request->header.from_nid,
						(struct pcn_kmsg_message*) reply,
						sizeof(*reply));

	MSGPRINTK("%s(): end\n", __func__);
	pcn_kmsg_free_msg(reply);
	return;
}

static void __handle_remote_thread_rdma_read_response(
										remote_thread_rdma_rw_t* inc_lmsg)
{
	remote_thread_rdma_rw_t* response =
								(remote_thread_rdma_rw_t*) inc_lmsg;
	struct krping_cb *_cb = cb[response->header.from_nid];

	CHECK_LOG("%s(): CHECK response->rdma_header.rw_size %d\n"
							"dummy_act_buf,(first10) %.10s\n"
							"dummy_act_buf(last 10) %.10s\n"
							"rdma_ack %s (check r_response, not here)\n\n\n",
							__func__, response->rdma_header.rw_size,
							dummy_act_buf,
							response->rdma_header.rw_size>10?
							dummy_act_buf+(response->rdma_header.rw_size-11):
							dummy_act_buf,
							response->rdma_header.rdma_ack?"true":"false");

#ifdef CONFIG_POPCORN_DEBUG_MSG_LAYER_VERBOSE
	DEBUG_LOG("response->rdma_hdr.remote_rkey %u remote_addr %p rw_size %u "
							"rw_t %d recv_ticket %lu ack_rdma_ticket %d\n",
									response->rdma_header.remote_rkey,
									(void*)response->rdma_header.remote_addr,
									response->rdma_header.rw_size,
									response->rw_ticket,		// rdma dbg
									response->header.ticket,	// send/recv dbg
									response->rdma_ticket); 	// rdma dbg
	MSG_SYNC_PRK("///////READ active unlock() %d rw_t %d conn %d///////////\n",
										(int)atomic_read(&_cb->active_cnt),
										response->rw_ticket,
										_cb->conn_no);
#endif

	//__unmap_act(_cb->conn_no, response->rdma_header.rw_size);
	mutex_unlock(&_cb->active_mutex);

	MSGPRINTK("%s(): end\n", __func__);
	return;
}


static void __handle_remote_thread_rdma_write_request(
						remote_thread_rdma_rw_t* inc_lmsg, void* target_paddr)
{
	remote_thread_rdma_rw_t* request =
								(remote_thread_rdma_rw_t*) inc_lmsg;
#if !CONFIG_FARM
	remote_thread_rdma_rw_t *reply;
#endif

#ifdef CONFIG_POPCORN_DEBUG_MSG_LAYER_VERBOSE
	unsigned long ts_wr_start, ts_wr_compose, ts_wr_post, ts_wr_end;
#endif
	struct krping_cb *_cb = cb[request->header.from_nid];
	struct ib_send_wr *bad_wr; // for ib_post_send
	int ret;

#ifdef CONFIG_POPCORN_DEBUG_MSG_LAYER_VERBOSE
	MSGPRINTK("<<<<< passive WRITE request: my %d from %d rw_t %d "
				"ticket %lu rdma_ticket %d  >>>>>\n",
				my_nid, request->header.from_nid, request->rw_ticket,
				request->header.ticket, request->rdma_ticket);
#endif
	/* ib client sending write key to [remote server] */
	// get key, and conjuct to the cb
	MSGDPRINTK("<<<<< rpc (remote request) r_write(remotely write)\n");

	/* send        ----->   irq (recv)
	 *                      [lock]
	 *             <=====   perform WRITE
	 *                      unlock
	 * irq (recv)  <-----   send
	 */

	mutex_lock(&_cb->passive_mutex);

#ifdef CONFIG_POPCORN_DEBUG_MSG_LAYER_VERBOSE
	atomic_inc(&_cb->passive_cnt);
	MSG_SYNC_PRK("/////////// WRITE passive lock() %d /////////////////\n",
										(int)atomic_read(&_cb->passive_cnt));
#endif

	_cb->passive_dma_addr = __ib_map_pass(target_paddr, _cb->conn_no,
												request->rdma_header.rw_size);

	// perform WRITE (passive side) + performance evaluation
	// <time1 : compose msg info>
#ifdef CONFIG_POPCORN_DEBUG_MSG_LAYER_VERBOSE
	rdtscll(ts_wr_start);
#endif

	/* RDMA WRITE echo data */
	// remote info:
	 _cb->remote_rkey = ntohl(request->rdma_header.remote_rkey); // redaundant
	 _cb->remote_addr = ntohll(request->rdma_header.remote_addr);// redaundant
	 _cb->remote_len = request->rdma_header.rw_size;			 // redaundant

	_cb->rdma_sq_wr.wr.opcode = IB_WR_RDMA_WRITE;
	_cb->rdma_sq_wr.rkey = _cb->remote_rkey;		// updated from remote!!!!
	_cb->rdma_sq_wr.remote_addr = _cb->remote_addr;	// updated from remote!!!!
	//_cb->rdma_sq_wr.wr.sg_list->length = _cb->remote_len;

	_cb->rdma_sq_wr.wr.next = NULL;					// one work request

	/*
	struct ib_send_wr inv;			// for ib_post_send
	_cb->rdma_sq_wr.wr.next = &inv;	// followed by a inv
	memset(&inv, 0, sizeof inv);
	inv.opcode = IB_WR_LOCAL_INV;
	inv.ex.invalidate_rkey = _cb->reg_mr_pass->rkey;
	inv.send_flags = IB_SEND_FENCE;
	*/

	// local info:
	// note: rdma_sq_wr.wr.sg_list = &cb->rdma_sgl
	_cb->rdma_sgl.length = _cb->remote_len;
	_cb->rdma_sgl.addr = _cb->passive_dma_addr;		// local
	// register local buf for performing R/W (rdma_rkey)
	_cb->rdma_sgl.lkey = __krping_rdma_rkey_pass(	// local
						_cb, _cb->passive_dma_addr, 1, _cb->remote_len);
						/*	In krping.c, they use 0 as the post_inv. */

	CHECK_LOG("<<<<< WRITE request: my_nid %d from_nid %d, "
						"lkey %d laddr %llx _cb->rdma_sgl.lkey %d, "
						"remote_rkey %d remote_addr %p rw_size %d>>>>>\n",
						my_nid, request->header.from_nid,
						_cb->rdma_sq_wr.wr.sg_list->lkey,
						(unsigned long long)_cb->rdma_sq_wr.wr.sg_list->addr,
						_cb->rdma_sgl.lkey, _cb->remote_rkey,
						(void*)_cb->remote_addr, _cb->remote_len);

	MSG_RDMA_PRK("ib_post_send W>>>>\n");
	// <time 2: send>
#ifdef CONFIG_POPCORN_DEBUG_MSG_LAYER_VERBOSE
	rdtscll(ts_wr_compose);
#endif

	mutex_lock(&_cb->qp_mutex);
	ret = ib_post_send(_cb->qp, &_cb->rdma_sq_wr.wr, &bad_wr);
	mutex_unlock(&_cb->qp_mutex);
	MSG_RDMA_PRK("ib_post_send W done>>>>\n");
	if (ret) {
		printk(KERN_ERR "post send error %d\n", ret);
		return;
	}
	/* If just sent a FENCE, this should be turned on */
	//_cb->rdma_sq_wr.wr.next = NULL;

	// <time 3: send request done>
#ifdef CONFIG_POPCORN_DEBUG_MSG_LAYER_VERBOSE
	rdtscll(ts_wr_post);
#endif

	/* Wait for completion */
	ret = wait_event_interruptible(_cb->sem,
					atomic_read(&_cb->write_state) == RDMA_WRITE_COMPLETE);
	atomic_set(&_cb->write_state, IDLE);
	// <time 4: WRITE(task) done>
#ifdef CONFIG_POPCORN_DEBUG_MSG_LAYER_VERBOSE
	rdtscll(ts_wr_end);
	/* time result */
	DEBUG_LOG("W: %d K compose_time %lu post_time %lu "
								"end_time %lu (cpu ticks)\n",
								(request->rdma_header.rw_size+1)/1024,
								ts_wr_compose-ts_wr_start, // +1 end char
								ts_wr_post-ts_wr_start, ts_wr_end-ts_wr_start);
#endif

	/* passive WRITE done */

	CHECK_LOG("<<<<< CHECK rpc (passive) R_WRITE DONE size %d\n"
						"_cb->rw_pass_buf(first10) \"%.10s\"\n"
						"_cb->rw_pass_buf(last 10) \"%.10s\"\n"
						"(check in response, not here)\n\n\n",
						request->rdma_header.rw_size,
						dummy_pass_buf,
						request->rdma_header.rw_size>10?
						dummy_pass_buf+(request->rdma_header.rw_size-11):
						dummy_pass_buf);

	//__unmap_pass(_cb->conn_no, request->rdma_header.rw_size);

	/* send ----->  irq
	 *				lock W
	 *      <=====  perform WRITE
	 *				[unlock W]
	 * irq  <-----  send
	 */

#ifdef CONFIG_POPCORN_DEBUG_MSG_LAYER_VERBOSE
	MSG_SYNC_PRK("///////////// WRITE passive unlock() %d /////////////////\n",
										(int)atomic_read(&_cb->passive_cnt));
#endif
	mutex_unlock(&_cb->passive_mutex);

#if !CONFIG_FARM
	/* send W completion ACK */
	DEBUG_LOG("send WRITE COMPLETION ACK\n");
	reply = pcn_kmsg_alloc_msg(sizeof(*reply));
	if (!reply)
		BUG_ON(-1);

	reply->header.type = request->rdma_header.rmda_type_res;
	reply->header.prio = PCN_KMSG_PRIO_NORMAL;
	//reply->tgroup_home_cpu = tgroup_home_cpu;
	//reply->tgroup_home_id = tgroup_home_id;

	// RDMA W/R complete ACK
	reply->header.is_rdma = true;
	reply->rdma_header.rdma_ack = true;
	reply->rdma_header.is_write = true;
	reply->rdma_header.remote_rkey = _cb->remote_rkey;
	reply->rdma_header.remote_addr = _cb->remote_addr;
	reply->rdma_header.rw_size = _cb->remote_len;

	/* for wait station */
	reply->remote_ws = inc_lmsg->remote_ws;

	__ib_kmsg_send_long(request->header.from_nid,
				(struct pcn_kmsg_message*) reply, sizeof(*reply));

	MSGPRINTK("%s(): end\n\n\n", __func__);
	pcn_kmsg_free_msg(reply);
#endif
	return;
}

static void __handle_remote_thread_rdma_write_response(
										remote_thread_rdma_rw_t* inc_lmsg)
{
#ifdef CONFIG_POPCORN_DEBUG_MSG_LAYER_VERBOSE
	remote_thread_rdma_rw_t* response =
								(remote_thread_rdma_rw_t*) inc_lmsg;
#endif

#if !CONFIG_FARM
	struct krping_cb *_cb = cb[response->header.from_nid];
#endif

#ifdef CONFIG_POPCORN_DEBUG_MSG_LAYER_VERBOSE
	CHECK_LOG("%s(): CHECK response->header.rw_size %d\n"
								"dummy_act_buf(first10) %.10s\n"
								"dummy_act_buf(last 10) %.10s\n"
				"rdma_ack %s(==true) (check here not w_request)\n\n\n",
								__func__, response->rdma_header.rw_size,
								dummy_act_buf,
								response->rdma_header.rw_size>10?
								dummy_act_buf+(response->rdma_header.rw_size-11):
								dummy_act_buf,
								response->rdma_header.rdma_ack?"true":"false");
#endif

#ifdef CONFIG_POPCORN_DEBUG_MSG_LAYER_VERBOSE
	DEBUG_LOG("CHECK response->header.remote_rkey %u remote_addr %p "
							"rw_size %u rw_t %d ticket %lu rdma_ticket %d\n",
										response->rdma_header.remote_rkey,
										(void*)response->rdma_header.remote_addr,
										response->rdma_header.rw_size,
										response->rw_ticket,
										response->header.ticket,
										response->rdma_ticket);
	MSG_SYNC_PRK("/////////////WRITE active unlock() %d////////////////\n",
											(int)atomic_read(&_cb->active_cnt));
#endif

#if !CONFIG_FARM
	//__unmap_act(_cb->conn_no, response->rdma_header.rw_size);
	mutex_unlock(&_cb->active_mutex);
#endif

	MSGPRINTK("%s(): end\n\n\n", __func__);
	return;
}

/*
 *paddr: ptr of pages you wanna perform on passive side
 */
void handle_rdma_request(remote_thread_rdma_rw_t *inc_lmsg, void *paddr)
{
	remote_thread_rdma_rw_t *lmsg = inc_lmsg;
	if (lmsg->header.is_rdma) {
		if (!lmsg->rdma_header.rdma_ack) {
			if (lmsg->rdma_header.is_write)
				__handle_remote_thread_rdma_write_request(lmsg, paddr);
			else
				__handle_remote_thread_rdma_read_request(lmsg, paddr);
		}
		else {
			if (lmsg->rdma_header.is_write)
				__handle_remote_thread_rdma_write_response(lmsg);
			else
				__handle_remote_thread_rdma_read_response(lmsg);
		}
	} else {
		printk(KERN_ERR "This is not a rdma request you shouldn't call"
						"\"pcn_kmsg_handle_remote_rdma_request\"\n"
						"from=%u, type=%d, msg_size=%u\n\n",
						lmsg->header.from_nid,
						lmsg->header.type,
						lmsg->header.size);
	}
}

/*
 * Pass msg to upper layer and do the corresponding callback function
 */
static void __pcn_kmsg_handler_BottomHalf(struct work_struct * work)
{
	pcn_kmsg_work_t *w = (pcn_kmsg_work_t *) work;
	struct pcn_kmsg_message *lmsg;
	pcn_kmsg_cbftn ftn;

	MSGPRINTK("%s(): \n", __func__);

	lmsg = w->lmsg;
	if ( lmsg->header.type < 0 || lmsg->header.type >= PCN_KMSG_TYPE_MAX) {
		printk(KERN_ERR "Received invalid message type %d > MAX %d\n",
									lmsg->header.type, PCN_KMSG_TYPE_MAX);
	} else {
		ftn = callbacks[lmsg->header.type];
		if (ftn != NULL) {
#ifdef CONFIG_POPCORN_MSG_STATISTIC
			atomic_inc(&recv_pattern[lmsg->header.size]);
#endif
			ftn((void*)lmsg);
		} else {
			MSGPRINTK(KERN_INFO "Recieved message type %d size %d "
								"has no registered callback!\n",
								lmsg->header.type, lmsg->header.size);
			pcn_kmsg_free_msg(lmsg);
			BUG();
		}
	}

	MSGPRINTK("%s(): done & free everything\n\n", __func__);
	kfree((void*)w);
	return;
}

/*
 * Parse recved msg in the buf to msg_layer in INT
 */
static int __ib_kmsg_recv_long(struct krping_cb *_cb,
							 struct wc_struct *wcs)
{
	struct pcn_kmsg_message *lmsg = wcs->element_addr;
	pcn_kmsg_work_t *kmsg_work;

	if (unlikely( lmsg->header.size > sizeof(struct pcn_kmsg_message))) {
		printk(KERN_ERR "Received invalide message size > MAX %lu\n",
									sizeof(struct pcn_kmsg_message));
		BUG();
	}

	DEBUG_LOG("%s(): producing BottomHalf wc->wr_id = lmsg %p header.size %d\n",
						__func__, (void*)lmsg, lmsg->header.size);

	// - alloc & cpy msg to kernel buffer
	kmsg_work = kmalloc(sizeof(pcn_kmsg_work_t), GFP_ATOMIC);
	if (unlikely(!kmsg_work)) {
		printk("Failed to kmalloc work structure!\n");
		BUG();
	}
	kmsg_work->lmsg = kmalloc(lmsg->header.size, GFP_ATOMIC);
	if (unlikely(!kmsg_work->lmsg)) {
		printk("Failed to kmalloc msg in work structure!\n");
		BUG();
	}

#ifdef CONFIG_POPCORN_DEBUG_MSG_LAYER_VERBOSE
	MSG_RDMA_PRK("bf: Spwning BottomHalf, leaving INT "
					"kwq->lmsg->header.type %d "
					"kwq->lmsg->header.size %d rw_t %d\n",
					lmsg->header.type,
					lmsg->header.size,
					lmsg->header.is_rdma?
					((remote_thread_rdma_rw_t*)lmsg)->rw_ticket:-1);
#endif
	if (unlikely(!memcpy(kmsg_work->lmsg, lmsg, lmsg->header.size)))
		BUG();

#ifdef CONFIG_POPCORN_DEBUG_MSG_LAYER_VERBOSE
	kmsg_work->lmsg->header.ticket = atomic_inc_return(&g_recv_ticket);
	MSGPRINTK("%s() recv ticket %lu\n",
						__func__, kmsg_work->lmsg->header.ticket);
#endif

#ifdef CONFIG_POPCORN_DEBUG_MSG_LAYER_VERBOSE
	MSG_RDMA_PRK("af: Spwning BottomHalf, leaving INT "
						"kwq->lmsg->header.size %d\n",
						kmsg_work->lmsg->header.type,
						kmsg_work->lmsg->header.size);
#endif

	INIT_WORK((struct work_struct *)kmsg_work, __pcn_kmsg_handler_BottomHalf);
	if (unlikely(!queue_work(msg_handler, (struct work_struct *)kmsg_work)))
		BUG();

	return 0;
}

static void __krping_cq_event_handler(struct ib_cq *cq, void *ctx)
{
	struct krping_cb *_cb = ctx;
	struct ib_wc wc; // work complition->wr_id (work request ID)
	struct ib_recv_wr *bad_wr;
	int ret;
	int i, recv_cnt = 0;
	struct ib_wc *_wc;

	MSGPRINTK("\n[[[[[external]]]]] node %d ------> %s\n",
									_cb->conn_no, __func__);

	BUG_ON(_cb->cq != cq);
	if (atomic_read(&_cb->state) == ERROR) {
		printk(KERN_ERR "< cq completion in ERROR state >\n");
		return;
	}

	/* get a completion */
	while ((ret = ib_poll_cq(_cb->cq, 1, &wc)) > 0) {
		_wc = &wc;

		if (_wc->status) { // !=IBV_WC_SUCCESS(0)
			if (_wc->status == IB_WC_WR_FLUSH_ERR) {
				MSGPRINTK("< cq flushed >\n");
			} else {
				printk(KERN_ERR "< cq completion failed with "
								"wr_id %Lx status %d opcode %d vender_err %x >"
								"!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n",
						_wc->wr_id, _wc->status, _wc->opcode, _wc->vendor_err);
				BUG_ON(_wc->status);
				goto error;
			}
		}

		switch (_wc->opcode) {
		case IB_WC_SEND:
#ifdef CONFIG_POPCORN_DEBUG_MSG_LAYER_VERBOSE
			atomic_inc(&_cb->stats.send_msgs);
			DEBUG_LOG("<<< --- from %d [[[[[ SEND ]]]]] COMPLETION %d --- >>>\n",
							_cb->conn_no, atomic_read(&_cb->stats.send_msgs));
#endif
			atomic_set(&_cb->send_state, RDMA_SEND_COMPLETE);
			wake_up_interruptible(&_cb->sem);
			break;

		case IB_WC_RDMA_WRITE:
#ifdef CONFIG_POPCORN_DEBUG_MSG_LAYER_VERBOSE
			atomic_inc(&_cb->stats.write_msgs);
			DEBUG_LOG("<<<<< ----- from %d [[[[[ RDMA WRITE ]]]]] "
							"COMPLETION %d ----- (good) >>>>>\n",
							_cb->conn_no, atomic_read(&_cb->stats.write_msgs));
#endif
			atomic_set(&_cb->write_state, RDMA_WRITE_COMPLETE);
			wake_up_interruptible(&_cb->sem);
			break;

		case IB_WC_RDMA_READ:
#ifdef CONFIG_POPCORN_DEBUG_MSG_LAYER_VERBOSE
			atomic_inc(&_cb->stats.read_msgs);
			DEBUG_LOG("<<<<< ----- from %d [[[[[ RDMA READ ]]]]] "
							"COMPLETION %d ----- (good) >>>>>\n",
							_cb->conn_no, atomic_read(&_cb->stats.read_msgs));
#endif
			atomic_set(&_cb->read_state, RDMA_READ_COMPLETE);
			wake_up_interruptible(&_cb->sem);
			break;

		case IB_WC_RECV:
			recv_cnt++;
			MSG_RDMA_PRK("ret %d recv_cnt %d\n", ret, recv_cnt);

#ifdef CONFIG_POPCORN_DEBUG_MSG_LAYER_VERBOSE
			atomic_inc(&_cb->stats.recv_msgs);
			DEBUG_LOG("<<< --- from %d [[[[[ RECV ]]]]] COMPLETION %d --- >>>\n",
							  _cb->conn_no, atomic_read(&_cb->stats.recv_msgs));
#endif

			ret = __ib_kmsg_recv_long(_cb, (struct wc_struct*)_wc->wr_id);
			if (ret) {
				printk(KERN_ERR "< recv wc error: %d >\n", ret);
				goto error;
			}
			__enq_wcs((struct wc_struct*)_wc->wr_id, _cb->conn_no);
			break;

		default:
			printk(KERN_ERR "< %s:%d Unexpected opcode %d, Shutting down >\n",
											__func__, __LINE__, _wc->opcode);
			goto error;
		}

		if (recv_cnt >= RECV_WQ_THRESHOLD)
			break;
	}

	for(i=0; i<recv_cnt; i++) {
		struct wc_struct *_wcs = __dq_wcs(_cb->conn_no);
		if(unlikely(!_wcs))
			break;

		ret = ib_post_recv(_cb->qp, _wcs->rq_wr, &bad_wr);

		if (ret) {
			printk(KERN_ERR "ib_post_recv failed: %d\n", ret);
			BUG();
		}
	}

	MSGPRINTK("\n[[[[[external done]]]]] node %d\n\n", _cb->conn_no);
	ib_req_notify_cq(_cb->cq, IB_CQ_NEXT_COMP);	// to arm CA to send eveent
											// on next completion added to CQ
	return;

error:
	atomic_set(&_cb->state, ERROR);
	wake_up_interruptible(&_cb->sem);
}



/*
 * Your request must be done by kmalloc().
 * You have to free by yourself
 *
 * rw_size: size you wanna passive remote to READ/WRITE
 */
int ib_kmsg_send_rdma(unsigned int dest_cpu, remote_thread_rdma_rw_t *lmsg,
					  unsigned int msg_size, unsigned int rw_size)
{
	uint32_t rkey;
	char *poll_tail_at;
	struct krping_cb *_cb = cb[dest_cpu];
	MSGDPRINTK("%s(): \n", __func__);

	if (!lmsg->rdma_header.your_buf_ptr || rw_size < 0)
		BUG();

	if (dest_cpu == my_nid) {
		printk(KERN_ERR "No support for sending msg to itself %d\n", dest_cpu);
		return 0;
	}

	lmsg->header.is_rdma = true;
	lmsg->header.from_nid = my_nid;
	lmsg->rdma_header.rdma_ack = false;
	lmsg->rdma_header.rw_size = rw_size;

	/* kmsg
	 * if R/W
	 * [lock]
	 * send		 ----->   irq (recv)
	 *					   |-lock R/W
	 *					   |-perform READ
	 *					   |-unlock R/W
	 * irq (recv)   <-----   |-send
	 *  |-unlock
	 */
	
	mutex_lock(&_cb->active_mutex);

	_cb->active_dma_addr = __ib_map_act( lmsg->rdma_header.your_buf_ptr,
													_cb->conn_no, rw_size);

#ifdef CONFIG_POPCORN_DEBUG_MSG_LAYER_VERBOSE
	lmsg->rw_ticket = atomic_inc_return(&_cb->g_all_ticket);
	atomic_inc(&_cb->active_cnt);
#endif

	/* form rdma meta data */
	rkey = __krping_rdma_rkey_act(_cb, _cb->active_dma_addr,
								!_cb->server_invalidate, rw_size);

	lmsg->rdma_header.remote_addr = htonll(_cb->active_dma_addr);
	lmsg->rdma_header.remote_rkey = htonl(rkey);
	CHECK_LOG("%s(): - @@@ cb[%d] rkey %d cb[]->active_dma_addr %p "
												"lmsg->rw_size %d\n",
												__func__, dest_cpu, rkey,
												(void*)_cb->active_dma_addr,
												 lmsg->rdma_header.rw_size);

	// pcn_msg (abstraction msg layer)
	//----------------------------------------------------------
	// ib
#ifdef CONFIG_POPCORN_DEBUG_MSG_LAYER_VERBOSE
	lmsg->rdma_ticket = atomic_inc_return(&g_rw_ticket); // from 1
	MSGPRINTK("%s(): rw ticket %d\n", __func__, lmsg->rdma_ticket);
#endif

	__ib_kmsg_send_long(dest_cpu, (struct pcn_kmsg_message*) lmsg, msg_size);

#if CONFIG_FARM
	poll_tail_at = lmsg->rdma_header.your_buf_ptr + rw_size - 1;
	/* poll at tail */
	while (*poll_tail_at == 0)
		schedule();
	mutex_unlock(&_cb->active_mutex);
#endif
	MSGPRINTK("%s(): Sent 1 rdma request\n", __func__);
	return 0;
}

void usr_rdma_poll_done(int dst_cpu)
{
	mutex_unlock(&cb[dst_cpu]->active_mutex);
}
EXPORT_SYMBOL(usr_rdma_poll_done);


int ib_kmsg_send_long(unsigned int dest_cpu,
					  struct pcn_kmsg_message *lmsg,
					  unsigned int msg_size)
{
	lmsg->header.is_rdma = false;
	return __ib_kmsg_send_long(dest_cpu, lmsg, msg_size);
}



// Initialize callback table to null, set up control and data channels
int __init initialize()
{
	int i, err, conn_no;
	msg_layer = "IB";

	KRPRINT_INIT("--- Popcorn messaging layer init starts ---\n");
	/*
	 * open_softirq(PCN_KMSG_SOFTIRQ, pcn_kmsg_action);
	 * 			regioster a handler run when a SOFTIRQ is triggered
	 */

	/* Essential checks */
	if (MAX_MSG_LENGTH > PCN_KMSG_LONG_PAYLOAD_SIZE) {
		printk(KERN_ERR "MAX_MSG_LENGTH %d shouldn't be larger than "
						"PCN_KMSG_LONG_PAYLOAD_SIZE %d\n",
						MAX_MSG_LENGTH, PCN_KMSG_LONG_PAYLOAD_SIZE);
		BUG();
	}

	/* init dummy buffers for geting experimental data */
	dummy_act_buf = kzalloc(MAX_MSG_LENGTH, GFP_KERNEL);
	KRPRINT_INIT("act_buf c %c d %u\n",
					(char)*(dummy_act_buf+MAX_MSG_LENGTH-1),
					(unsigned int)((char)*(dummy_act_buf+MAX_MSG_LENGTH-1)));
	dummy_pass_buf = kzalloc(MAX_MSG_LENGTH, GFP_KERNEL);
	if (!dummy_act_buf || !dummy_pass_buf)
		BUG();
	memset(dummy_act_buf, 'A', 10);
	memset(dummy_act_buf+10, 'B', MAX_MSG_LENGTH-10);
	memset(dummy_pass_buf, 'P', 10);
	memset(dummy_pass_buf+10, 'Q', MAX_MSG_LENGTH-10);

	/* Establish node numbers with ip */
	if (!init_ip_table()) return -EINVAL;

	/* Create a workqueue for bottom-half */
	msg_handler = create_workqueue("MSGHandBotm"); // per-cpu

	KRPRINT_INIT("-------------------------------------------------\n");
	KRPRINT_INIT("---- updating to my_nid=%d wait for a moment ----\n", my_nid);
	KRPRINT_INIT("-------------------------------------------------\n");
	KRPRINT_INIT("MSG_LAYER: Initialization my_nid=%d\n", my_nid);

	/* Initilaize the IB -
	 * Each node has a connection table like tihs:
	 * -------------------------------------------------------------------
	 * | connect | (many)... | my_nid(one) | accept | accept | (many)... |
	 * -------------------------------------------------------------------
	 * my_nid:  no need to talk to itself
	 * connect: connecting to existing nodes
	 * accept:  waiting for the connection requests from later nodes
	 */
	for (i=0; i<MAX_NUM_NODES; i++) {
		/* 0. create cb context for each connection */
		cb[i] = kzalloc(sizeof(struct krping_cb), GFP_KERNEL);

		/* settup node number */
		conn_no = i;
		cb[i]->conn_no = i;
		set_popcorn_node_online(i, false);

		/* setup locks */
		mutex_init(&cb[i]->send_mutex);
		mutex_init(&cb[i]->recv_mutex);
		mutex_init(&cb[i]->active_mutex);
		mutex_init(&cb[i]->passive_mutex);
		mutex_init(&cb[i]->qp_mutex);

		/* 1. init comment parameters */
		cb[i]->state.counter = IDLE;
		cb[i]->send_state.counter = IDLE;
		cb[i]->read_state.counter = IDLE;
		cb[i]->write_state.counter = IDLE;

#ifdef CONFIG_POPCORN_DEBUG_MSG_LAYER_VERBOSE
		cb[i]->active_cnt.counter = 0;
		cb[i]->passive_cnt.counter = 0;
#endif

		/* For RECV wc_struct queue */
        INIT_LIST_HEAD(&wcs_wait_q[i].list);
        init_completion(&(wcs_comp[i]));
		spin_lock_init(&(wcs_q_mutex[i]));

#ifdef CONFIG_POPCORN_DEBUG_MSG_LAYER_VERBOSE
		g_rw_ticket.counter = 0;
		g_send_ticket.counter = 0;
		g_recv_ticket.counter = 0;
#endif

#ifdef CONFIG_POPCORN_DEBUG_MSG_LAYER_VERBOSE
		/* for sync problem */
		cb[i]->g_all_ticket.counter = 0;
#endif

#ifdef CONFIG_POPCORN_DEBUG_MSG_LAYER_VERBOSE
		cb[i]->stats.send_msgs.counter = 0;
		cb[i]->stats.recv_msgs.counter = 0;
		cb[i]->stats.write_msgs.counter = 0;
		cb[i]->stats.read_msgs.counter = 0;
#endif

		cb[i]->rdma_size = MAX_RDMA_SIZE;

		init_waitqueue_head(&cb[i]->sem); // init waitq for wait, wake up
		cb[i]->txdepth = RPING_SQ_DEPTH;

		/* 2. init uncommon parameters */
		// server/client/myself
		cb[i]->server = -1;

		/* set up IPv4 address */
		cb[i]->addr_str = ip_addresses[conn_no];
		in4_pton(ip_addresses[conn_no], -1, cb[i]->addr, -1, NULL);
		cb[i]->addr_type = AF_INET;		// [IPv4]/V6 // for determining
		cb[i]->port = htons(PORT);
		KRPRINT_INIT("ip_addresses[conn_no] %s, cb[i]->addr_str %s, "
										 "cb[i]->addr %s,  port %d\n",
												 ip_addresses[conn_no],
												 cb[i]->addr_str,
												 cb[i]->addr, (int)PORT);

		/* register event handler */
		cb[i]->cm_id = rdma_create_id(&init_net, __krping_cma_event_handler,
											cb[i], RDMA_PS_TCP, IB_QPT_RC);
		if (IS_ERR(cb[i]->cm_id)) {
			err = PTR_ERR(cb[i]->cm_id);
			printk(KERN_ERR "rdma_create_id error %d\n", err);
			goto out;
		}
		KRPRINT_INIT("created cm_id %p (pair to event handler)\n",
															cb[i]->cm_id);

		cb[i]->recv_size = sizeof(struct pcn_kmsg_message);

		cb[i]->server_invalidate = 0;
		cb[i]->read_inv = 0;
	}
	KRPRINT_INIT("---- main init done (still cannot send/recv) -----\n\n");

	/* Initilaize the sock */
	/*
	 *  Each node has a connection table like tihs:
	 * -------------------------------------------------------------------
	 * | connect | (many)... | my_nid(one) | accept | accept | (many)... |
	 * -------------------------------------------------------------------
	 * my_nid:  no need to talk to itself
	 * connect: connecting to existing nodes
	 * accept:  waiting for the connection requests from later nodes
	 */

	cb[my_nid]->server = 1;
	set_popcorn_node_online(my_nid, true);

	//- client -//
	/* 1. [<my_nid: connect] / >=my_nid: accept */
	for (i=0; i<MAX_NUM_NODES; i++) {
		if (i == my_nid)
			continue; // has done (server)

		conn_no = i;	 // Take node 1 for example. connect0 [1] accept2
		if (conn_no < my_nid) { // 1. my_nid(accept), [connect]
			cb[conn_no]->server = 0;

			// server/client dependant init
			if (__krping_run_client(cb[conn_no])) {
				printk("WRONG!!\n");
				rdma_disconnect(cb[conn_no]->cm_id);
				return err;
			}

			set_popcorn_node_online(conn_no, true);
			smp_mb(); // Jack: calling it one time in the end should be fine
			printk("conn_no %d is ready (GOOD)\n", conn_no);
		}
		else{
			KRPRINT_INIT("no action needed for conn %d "
						  "(listening will take care)\n", i);
		}
		//sched_setscheduler(<kthread_run()'s return>, SCHED_FIFO, &param);
		//set_cpus_allowed_ptr(<kthread_run()'s return>, cpumask_of(i%NR_CPUS));
	}

	/* 2. <my_nid: connect / [>=my_nid: accept] */
	__krping_run_server(cb[my_nid]);


	for ( i=0; i<MAX_NUM_NODES; i++ ) {
		while ( !get_popcorn_node_online(i) ) {
			printk("waiting for get_popcorn_node_online(%d)\n", i);
			msleep(3000);
		}
	}

#if DEMON_POST_RECV_WCS
	for (i=0; i<MAX_NUM_NODES; i++) {
		if (i == my_nid)
			continue;
		t = kthread_run(__recv_post, cb[i],
								"demon for recyclingly posting recv_wc");
		BUG_ON(IS_ERR(t));
	}
#endif

	MSGPRINTK("--- init all ib[]->state ---\n");
	for ( i=0; i<MAX_NUM_NODES; i++ ) {
		atomic_set(&cb[i]->state, IDLE);
		atomic_set(&cb[i]->send_state, IDLE);
		atomic_set(&cb[i]->read_state, IDLE);
		atomic_set(&cb[i]->write_state, IDLE);
	}

	/* testing code is in another module, msg_layer_test.ko */

	send_callback = (send_cbftn)ib_kmsg_send_long;
	send_callback_rdma = (send_rdma_cbftn)ib_kmsg_send_rdma;
	handle_rdma_callback = (handle_rdma_request_ftn)handle_rdma_request;
	MSGPRINTK("Value of send ptr = %p\n", send_callback);
	MSGPRINTK("--- Popcorn messaging layer is up ---\n");

	smp_mb();
	printk("==================================================\n");
	printk("----- Popcorn Messaging Layer IB Initialized -----\n");
	printk("==================================================\n"
														"\n\n\n\n\n\n\n");
	return 0;

out:
	for(i=0; i<MAX_NUM_NODES; i++){
		if (atomic_read(&cb[i]->state)) {
			kfree(cb[i]);
			// TODO: cut connections
		}
	}
	return err;
}


/*
 *  Not yet done.
 */
static void __exit unload(void)
{
	int i;
	KRPRINT_INIT("Stopping kernel threads\n");

	/* TODO: rdma_disconnect() */

	for (i=0; i<MAX_NUM_NODES; i++) {
		kfree(cb[i]);
	}

	KRPRINT_INIT("Release generals\n");
	for (i = 0; i<MAX_NUM_NODES; i++) {
	}

	/* release */
	KRPRINT_INIT("Release threadss\n");
	for (i = 0; i<MAX_NUM_NODES; i++) {
		printk(KERN_ERR "Not implemented yet\n");
	}

	KRPRINT_INIT("Release IBs\n");
	for (i = 0; i<MAX_NUM_NODES; i++) {
	}

	KRPRINT_INIT("Successfully unloaded module!\n");
}

module_init(initialize);
module_exit(unload);
MODULE_LICENSE("GPL");
