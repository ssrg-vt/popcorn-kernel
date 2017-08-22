/*
 * msg_ib.c - Kernel Module for Popcorn Messaging Layer
 * multi-node version over InfiniBand
 * Author: Ho-Ren(Jack) Chuang
 *
 * TODO:
 *		define 0~1 to enum if needed
 *		(perf!)sping when send
 *		RDMA:
 *			remove mutex in READ/WRITE (static wr)
 * 			support multithreading FaRM2 by bmap polling slots
 *			implemet a wait/wakup in exchaging rkey
 */

// TODO: TESTING BUFF ALLOCATION 22

#include <linux/module.h>
#include <linux/kernel.h>
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

/* RDMA */
#include <rdma/ib_verbs.h>
#include <rdma/rdma_cm.h>

/* page */
#include <linux/pagemap.h>
#include <popcorn/stat.h>

#include "common.h"

/* features been developed */
#define CONFIG_FARM 0		/* Original FaRM - user follows convention  */
#define CONFIG_FARM_COPY 0	/* with 1 buf copy */
#define CONFIG_FARM2WRITE 0	/* 2 WRITE */

/* IB recv */
#define MAX_RECV_WR 1024	/* important! Check it if only sender crash */

/* IB send & completetion */
#define MAX_SEND_WR 1024
#define MAX_CQE MAX_SEND_WR + MAX_RECV_WR

/* RECV_BUF_POOL */
#define MAX_RECV_WORK_POOL MAX_RECV_WR

/* RDMA MR POOL */
//#define MAX_MR_SIZE 32
#define MAX_MR_SIZE 16

/* IB qp */
#define MAX_RECV_SGE 1
#define MAX_SEND_SGE 1

/* FaRM: w/ 1 extra copy version of FaRM */
#if CONFIG_FARM_COPY
#define FaRM_HEAD 4 + 1	/* length + length end bit*/
#define FaRM_TAIL 1
#define FaRM_HEAD_AND_TAIL FaRM_HEAD + FaRM_TAIL

#define FaRM_IS_DATA 0x01
#define FaRM_IS_EMPTY 0xff
#endif

#define FaRM_IS_IDLE 0

/* IB buffers */
#define MAX_KMALLOC_SIZE 1 * 1024 * 1024	/* limited by MAX_ORDER (4M now)*/
#if CONFIG_FARM_COPY
#define MAX_RDMA_SIZE MAX_KMALLOC_SIZE - FaRM_HEAD_AND_TAIL
#else
#define MAX_RDMA_SIZE MAX_KMALLOC_SIZE
#endif

/* FaRM2: two WRITE version of FaRM */
#if CONFIG_FARM2WRITE
#define MAX_SGE_NUM 1
#define FaRM2_ACT_DATA_SIZE MAX_SEND_WR
#define FaRM2_PASS_DATA_SIZE 1
#define MAX_RDMA_FARM2_SIZE 1
#endif

/* INT */
#define INT_MASK 0

/* IB connection config */
#define PORT 1000
#define LISTEN_BACKLOG 99
#define CONN_RESPONDER_RESOURCES 1
#define CONN_INITIATOR_DEPTH 1
#define CONN_RETRY_CNT 1
#define htonll(x) cpu_to_be64((x))
#define ntohll(x) cpu_to_be64((x))

/* RDMA key register */
#define RDMA_RKEY_ACT 0
#define RDMA_RKEY_PASS 1
#define RDMA_FARM2_RKEY_ACT 2
#define RDMA_FARM2_RKEY_PASS 3
#define RDMA_LAST_RKEY_MODE 4

/* IB runtime status */
#define IDLE 1
#define CONNECT_REQUEST 2
#define ADDR_RESOLVED 3
#define ROUTE_RESOLVED 4
#define CONNECTED 5
#define ERROR 6

/* workqueue arg */
typedef struct {
	struct work_struct work;	/* Convention! */
	int id;
	struct ib_sge *recv_sgl;
	struct ib_recv_wr *recv_wr;
	struct pcn_kmsg_message msg;
} ib_recv_work_t;

/* FaRM2 */
#if CONFIG_FARM2WRITE
struct FaRM2_init_req_t {
    struct pcn_kmsg_hdr header;
    uint32_t remote_rkey;
    uint64_t remote_addr;
    //uint32_t remote_size;
};
#endif

/* InfiniBand Control Block */
struct ib_cb {
	/* Parameters */
	int server;						/* server:1/client:0/myself:-1 */
	int conn_no;
	int read_inv;
	int server_invalidate;
	u8 key;

	/* IB essentials */
	struct ib_cq *cq;				/* can split into two send/recv */
	struct ib_pd *pd;
	struct ib_qp *qp;

	/* how many WR in Work Queue */
	atomic_t WQ_WR_cnt;

	/* Send buffer */
	struct ib_send_wr send_wr;		/* send work requrest info for send */
	struct ib_sge send_sgl;
	u64 send_dma_addr;
	u64 send_mapping;				/* for unmapping */

	/* RDMA common */
	struct ib_mr *reg_mr_act[MAX_MR_SIZE];
	struct ib_mr *reg_mr_pass[MAX_MR_SIZE];

	struct ib_reg_wr reg_mr_wr_act[MAX_MR_SIZE];	/* reg kind of = rdma  */
	struct ib_reg_wr reg_mr_wr_pass[MAX_MR_SIZE];
	struct ib_send_wr inv_wr_act[MAX_MR_SIZE];
	struct ib_send_wr inv_wr_pass[MAX_MR_SIZE];

	/* RDMA local info */
	u64 rdma_mapping_act;
	u64 rdma_mapping_pass;
#if CONFIG_FARM_COPY
	char *FaRM_pass_buf[MAX_MR_SIZE];			/* reuse passive buffer to perform WRITE */
#endif

#if CONFIG_FARM2WRITE
	struct ib_mr *reg_FaRM2_mr_act;
	struct ib_mr *reg_FaRM2_mr_pass[MAX_MR_SIZE];

	struct ib_reg_wr reg_FaRM2_mr_wr_act;
	struct ib_reg_wr reg_FaRM2_mr_wr_pass[MAX_MR_SIZE];
	struct ib_send_wr inv_FaRM2_wr_act;
	struct ib_send_wr inv_FaRM2_wr_pass[MAX_MR_SIZE];

	/* From remote */
	uint32_t remote_FaRM2_rkey;
	uint64_t remote_FaRM2_raddr;
	//uint32_t remote_FaRM2_rlen;
	/* From locaol */
	uint32_t local_FaRM2_lkey[MAX_MR_SIZE];
	uint64_t local_FaRM2_laddr[MAX_MR_SIZE];
	//uint32_t local_FaRM2_llen;

	/* RDMA buf for FaRM2 (local) */
	char *FaRM2_buf_act;
	char *FaRM2_buf_pass[MAX_MR_SIZE];
	u64 FaRM2_dma_addr_act;
	u64 FaRM2_dma_addr_pass[MAX_MR_SIZE];
#endif

	/* Connection */
	u8 addr[16];				/* dst addr in NBO */
	const char *addr_str;		/* dst addr string */
	uint8_t addr_type;			/* ADDR_FAMILY - IPv4/V6 */

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
	struct mutex active_mutex;
	struct mutex passive_mutex;	/* passive lock*/
	struct mutex qp_mutex;		/* protect ib_post_send(qp) */
};

/* InfiniBand Control Block per connection*/
struct ib_cb *gcb[MAX_NUM_NODES];
/* workqueue */
struct workqueue_struct *msg_handler;

/* Functions */
static int __init initialize(void);
static void ib_cq_event_handler(struct ib_cq *cq, void *ctx);

/* Popcorn utilities */
extern char *msg_layer;
extern pcn_kmsg_cbftn callbacks[PCN_KMSG_TYPE_MAX];
extern send_cbftn send_callback;
extern send_rdma_cbftn send_rdma_callback;
extern handle_rdma_request_ftn handle_rdma_callback;
extern kmsg_free_ftn kmsg_free_callback;

/* MR bit map */
static spinlock_t mr_avail_bmap_lock[MAX_NUM_NODES][RDMA_LAST_RKEY_MODE];
static unsigned long mr_poll_slot_avail[MAX_NUM_NODES][RDMA_LAST_RKEY_MODE]
										[BITS_TO_LONGS(MAX_MR_SIZE)];
/* Wrapped by a sem for reducing cpu usage? */
static u32 get_mr_ofs(int dst, int mode)
{
    int ofs;
retry:
    spin_lock(&mr_avail_bmap_lock[dst][mode]);
		ofs = find_first_zero_bit(mr_poll_slot_avail[dst][mode], MAX_MR_SIZE);
	if (ofs == MAX_MR_SIZE) {
		spin_unlock(&mr_avail_bmap_lock[dst][mode]);
		goto retry;
	}

    set_bit(ofs, mr_poll_slot_avail[dst][mode]);
    spin_unlock(&mr_avail_bmap_lock[dst][mode]);
    return ofs;
}

static void put_mr_ofs(int dst, u32 ofs, int mode)
{
    spin_lock(&mr_avail_bmap_lock[dst][mode]);
    clear_bit(ofs, mr_poll_slot_avail[dst][mode]);
    spin_unlock(&mr_avail_bmap_lock[dst][mode]);
}

#if CONFIG_FARM2WRITE
/* FaRM2 multiple polling slots */
static spinlock_t FaRM2_avail_bmap_lock[MAX_NUM_NODES][2];
static unsigned long FaRM2_poll_slot_avail[MAX_NUM_NODES][2]
								[BITS_TO_LONGS(MAX_MR_SIZE)];
static u32 get_send_wr_comp(int dst, int mode)
{
    int ofs;

retry2:
    spin_lock(&FaRM2_avail_bmap_lock[dst][mode]);
    ofs = find_first_zero_bit(FaRM2_poll_slot_avail[dst][mode], MAX_MR_SIZE);
	if (ofs == MAX_MR_SIZE) {
		spin_unlock(&FaRM2_avail_bmap_lock[dst][mode]);
		goto retry2;
	}
    set_bit(ofs, FaRM2_poll_slot_avail[dst][mode]);
    spin_unlock(&FaRM2_avail_bmap_lock[dst][mode]);
    return ofs;
}

static void put_send_wr_comp(int dst, u32 ofs, int mode)
{
    spin_lock(&FaRM2_avail_bmap_lock[dst][mode]);
    clear_bit(ofs, FaRM2_poll_slot_avail[dst][mode]);
    spin_unlock(&FaRM2_avail_bmap_lock[dst][mode]);
}
#endif

static int ib_cma_event_handler(struct rdma_cm_id *cma_id,
										struct rdma_cm_event *event)
{
	int ret;
	struct ib_cb *cb = cma_id->context; /* use cm_id to retrive cb */
	static int cma_event_cnt = 0, conn_event_cnt = 0;

	switch (event->event) {
	case RDMA_CM_EVENT_ADDR_RESOLVED:
		atomic_set(&cb->state, ADDR_RESOLVED);
		ret = rdma_resolve_route(cma_id, 2000);
		if (ret) {
			printk(KERN_ERR "< rdma_resolve_route error %d >\n", ret);
			wake_up_interruptible(&cb->sem);
		}
		break;

	case RDMA_CM_EVENT_ROUTE_RESOLVED:
		atomic_set(&cb->state, ROUTE_RESOLVED);
		wake_up_interruptible(&cb->sem);
		break;

	case RDMA_CM_EVENT_CONNECT_REQUEST:
		atomic_set(&cb->state, CONNECT_REQUEST);
		/* distributed to other connections */
		cb->child_cm_id = cma_id;
		wake_up_interruptible(&cb->sem);
		break;

	case RDMA_CM_EVENT_ESTABLISHED:
		if (gcb[my_nid]->conn_no == cb->conn_no) {
			cma_event_cnt++;

			atomic_set(&gcb[my_nid + cma_event_cnt]->state, CONNECTED);
			wake_up_interruptible(&gcb[my_nid + cma_event_cnt]->sem);
		} else {
			atomic_set(&gcb[conn_event_cnt]->state, CONNECTED);
			wake_up_interruptible(&gcb[conn_event_cnt]->sem);
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
		atomic_set(&cb->state, ERROR);
		wake_up_interruptible(&cb->sem);
		break;

	case RDMA_CM_EVENT_DISCONNECTED:
		printk(KERN_ERR "< --- DISCONNECT EVENT conn %d --- >\n", cb->conn_no);
		//atomic_set(&cb->state, ERROR);	// TODO for rmmod
		/* Current implementation:  free all resources in __exit */
		wake_up_interruptible(&cb->sem);
		break;

	case RDMA_CM_EVENT_DEVICE_REMOVAL:
		printk(KERN_ERR "< -----cma detected device removal!!!!----- >\n");
		break;

	default:
		printk(KERN_ERR "< -----oof bad type!----- >\n");
		wake_up_interruptible(&cb->sem);
		break;
	}
	return 0;
}

/*
 * Create a recv scatter-gather list(entries) & work request
 */
void create_recv_wr(int conn_no, ib_recv_work_t *kmsg_work)
{
	struct ib_sge *_recv_sgl;
	struct ib_recv_wr *_recv_wr;
	struct ib_cb *cb = gcb[conn_no];
	int recv_size = PCN_KMSG_MAX_SIZE;

	_recv_sgl = kzalloc(sizeof(*_recv_sgl), GFP_KERNEL);
	BUG_ON(!_recv_sgl && "sgl recv_buf malloc failed");

	_recv_wr = kzalloc(sizeof(*_recv_wr), GFP_KERNEL);
	BUG_ON(!_recv_wr && "recv_wr recv_buf malloc failed");

	/* set up sgl */
	_recv_sgl->length = recv_size;
	_recv_sgl->lkey = cb->pd->local_dma_lkey;
	_recv_sgl->addr = dma_map_single(cb->pd->device->dma_device,
					  &kmsg_work->msg, recv_size, DMA_BIDIRECTIONAL);

	/* set up recv_wr */
	_recv_wr->sg_list = _recv_sgl;
	_recv_wr->num_sge = 1;
	_recv_wr->wr_id = (u64)kmsg_work;
	_recv_wr->next = NULL;

	kmsg_work->recv_wr =  _recv_wr;
	kmsg_work->recv_sgl = _recv_sgl;
}


static int ib_connect_client(struct ib_cb *cb)
{
	int ret;
	struct rdma_conn_param conn_param;

	memset(&conn_param, 0, sizeof conn_param);
	conn_param.responder_resources = CONN_RESPONDER_RESOURCES;
	conn_param.initiator_depth = CONN_INITIATOR_DEPTH;
	conn_param.retry_count = CONN_RETRY_CNT;

	ret = rdma_connect(cb->cm_id, &conn_param);
	if (ret) {
		printk(KERN_ERR "rdma_connect error %d\n", ret);
		return ret;
	}

	wait_event_interruptible(cb->sem,
							atomic_read(&cb->state) == CONNECTED);
	if (atomic_read(&cb->state) == ERROR) {
		printk(KERN_ERR "wait for CONNECTED state %d\n",
								atomic_read(&cb->state));
		return -1;
	}
	return 0;
}

static void fill_sockaddr(struct sockaddr_storage *sin, struct ib_cb *cb)
{
	memset(sin, 0, sizeof(*sin));

	if (!cb->server) {
		/* client: load as usuall (ip=remote) */
		if (cb->addr_type == AF_INET) {
			struct sockaddr_in *sin4 = (struct sockaddr_in *)sin;
			sin4->sin_family = AF_INET;
			memcpy((void *)&sin4->sin_addr.s_addr, cb->addr, 4);
			sin4->sin_port = htons(PORT);
		}
	} else {
		/* cb->server: load from global (ip=itself) */
		if (gcb[my_nid]->addr_type == AF_INET) {
			struct sockaddr_in *sin4 = (struct sockaddr_in *)sin;
			sin4->sin_family = AF_INET;
			memcpy((void *)&sin4->sin_addr.s_addr, gcb[my_nid]->addr, 4);
			sin4->sin_port = htons(PORT);
		}
	}
}

static int ib_bind_server(struct ib_cb *cb)
{
	int ret;
	struct sockaddr_storage sin;

	fill_sockaddr(&sin, cb);
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

/* set up sgl */
static void ib_setup_wr(struct ib_cb *cb)
{
	int i = 0, ret;

	/* Pre-post RECV buffers */
	for(i = 0; i < MAX_RECV_WR; i++) {
		struct ib_recv_wr *bad_wr;
		ib_recv_work_t *kmsg_work = kzalloc(sizeof(*kmsg_work), GFP_KERNEL);
		BUG_ON(!kmsg_work);

		kmsg_work->id = i;
		create_recv_wr(cb->conn_no, kmsg_work);
		BUG_ON(!kmsg_work->recv_wr);

		ret = ib_post_recv(cb->qp, kmsg_work->recv_wr, &bad_wr);
		BUG_ON(ret && "ib_post_recv failed");
	}

	for (i = 0; i < MAX_MR_SIZE; i++) {
		/*
		 * A chain of 2 WRs, INVALDATE_MR + REG_MR.
		 * both unsignaled (no completion).  The client uses them to reregister
		 * the rdma buffers with a new key each iteration.
		 * IB_WR_REG_MR = legacy:fastreg mode
		 */
		cb->reg_mr_wr_act[i].wr.opcode = IB_WR_REG_MR;
		cb->reg_mr_wr_act[i].mr = cb->reg_mr_act[i];
		cb->reg_mr_wr_pass[i].wr.opcode = IB_WR_REG_MR;
		cb->reg_mr_wr_pass[i].mr = cb->reg_mr_pass[i];

#if CONFIG_FARM2WRITE
		cb->reg_FaRM2_mr_wr_pass[i].wr.opcode = IB_WR_REG_MR;
		cb->reg_FaRM2_mr_wr_pass[i].mr = cb->reg_FaRM2_mr_pass[i];
#endif

		/*
		 * 1. invalidate Memory Window locally
		 * 2. then register this new key to mr
		 */
		cb->inv_wr_act[i].opcode = IB_WR_LOCAL_INV;
		cb->inv_wr_act[i].next = &cb->reg_mr_wr_act[i].wr;
		cb->inv_wr_pass[i].opcode = IB_WR_LOCAL_INV;
		cb->inv_wr_pass[i].next = &cb->reg_mr_wr_pass[i].wr;
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

#if CONFIG_FARM2WRITE
		cb->inv_FaRM2_wr_pass[i].opcode = IB_WR_LOCAL_INV;
		cb->inv_FaRM2_wr_pass[i].next = &cb->reg_FaRM2_mr_wr_pass[i].wr;
#endif
	}
#if CONFIG_FARM2WRITE
	cb->reg_FaRM2_mr_wr_act.wr.opcode = IB_WR_REG_MR;
	cb->reg_FaRM2_mr_wr_act.mr = cb->reg_FaRM2_mr_act;

	cb->inv_FaRM2_wr_act.opcode = IB_WR_LOCAL_INV;
	cb->inv_FaRM2_wr_act.next = &cb->reg_FaRM2_mr_wr_act.wr;
#endif
	return;
}

static int _ib_create_qp(struct ib_cb *cb)
{
	int ret;
	struct ib_qp_init_attr init_attr;

	memset(&init_attr, 0, sizeof(init_attr));

	/* send and recv queue depth */
	init_attr.cap.max_send_wr = MAX_SEND_WR;
	init_attr.cap.max_recv_wr = MAX_RECV_WR * 2;

	/* For flush_qp() */
	init_attr.cap.max_send_wr++;
	init_attr.cap.max_recv_wr++;

	init_attr.cap.max_recv_sge = MAX_RECV_SGE;
	init_attr.cap.max_send_sge = MAX_SEND_SGE;
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

static int ib_setup_qp(struct ib_cb *cb, struct rdma_cm_id *cm_id)
{
	int ret;
	struct ib_cq_init_attr attr = {0};

	cb->pd = ib_alloc_pd(cm_id->device);
	if (IS_ERR(cb->pd)) {
		printk(KERN_ERR "ib_alloc_pd failed\n");
		return PTR_ERR(cb->pd);
	}

	attr.cqe = MAX_CQE;
	attr.comp_vector = INT_MASK;
	cb->cq = ib_create_cq(cm_id->device,
							ib_cq_event_handler, NULL, cb, &attr);
	if (IS_ERR(cb->cq)) {
		printk(KERN_ERR "ib_create_cq failed\n");
		ret = PTR_ERR(cb->cq);
		goto err1;
	}

	/* to arm CA to send eveent on next completion added to CQ */
	ret = ib_req_notify_cq(cb->cq, IB_CQ_NEXT_COMP);
	if (ret) {
		printk(KERN_ERR "ib_create_cq failed\n");
		goto err2;
	}

	ret = _ib_create_qp(cb);
	if (ret) {
		printk(KERN_ERR "ib_create_qp failed: %d\n", ret);
		goto err2;
	}
	return 0;
err2:
	ib_destroy_cq(cb->cq);
err1:
	ib_dealloc_pd(cb->pd);
	return ret;
}

/*
 * register local buf for performing R/W (rdma_rkey)
 * Return the (possibly rebound) rkey for the rdma buffer.
 * REG mode: invalidate and rebind via reg wr.
 * Other modes: just return the mr rkey.
 *
 * mode:
 *		0:RDMA_RKEY_ACT
 *		1:RDMA_RKEY_PASS
 *		2:RDMA_FARM2_RKEY_ACT
 *		3:RDMA_FARM2_RKEY_PASS
 */
static u32 ib_rdma_rkey(struct ib_cb *cb, u64 buf, int post_inv,
						int rdma_len, u32 mr_ofs, int mode)
{
	int ret;
	struct ib_send_wr *bad_wr;
	struct scatterlist sg = {0};
	struct ib_mr *reg_mr;

	struct ib_send_wr *inv_wr;
	struct ib_reg_wr *reg_mr_wr;

	if (mode == RDMA_RKEY_ACT) {
		reg_mr = cb->reg_mr_act[mr_ofs];
		inv_wr = &cb->inv_wr_act[mr_ofs];
		reg_mr_wr = &cb->reg_mr_wr_act[mr_ofs];
	} else if (mode == RDMA_RKEY_PASS) {
		reg_mr = cb->reg_mr_pass[mr_ofs];
		inv_wr = &cb->inv_wr_pass[mr_ofs];
		reg_mr_wr = &cb->reg_mr_wr_pass[mr_ofs];
#if CONFIG_FARM2WRITE
	} else if (mode == RDMA_FARM2_RKEY_ACT) {
		reg_mr = cb->reg_FaRM2_mr_act;
		inv_wr = &cb->inv_FaRM2_wr_act;
		reg_mr_wr = &cb->reg_FaRM2_mr_wr_act;
	} else if (mode == RDMA_FARM2_RKEY_PASS) {
		reg_mr = cb->reg_FaRM2_mr_pass[mr_ofs];
		inv_wr = &cb->inv_FaRM2_wr_pass[mr_ofs];
		reg_mr_wr = &cb->reg_FaRM2_mr_wr_pass[mr_ofs];
#endif
	}

	inv_wr->ex.invalidate_rkey = reg_mr->rkey;
	ib_update_fast_reg_key(reg_mr, cb->key);
	reg_mr_wr->key = reg_mr->rkey;

	reg_mr_wr->access = IB_ACCESS_REMOTE_READ	|
						IB_ACCESS_REMOTE_WRITE	|
						IB_ACCESS_LOCAL_WRITE	|
						IB_ACCESS_REMOTE_ATOMIC;

	sg_dma_address(&sg) = buf;
	sg_dma_len(&sg) = rdma_len;

	ret = ib_map_mr_sg(reg_mr, &sg, 1, PAGE_SIZE);
			// snyc: use ib_dma_sync_single_for_cpu/dev dev:accessed by IB
	BUG_ON(ret <= 0 || ret > ((((MAX_RDMA_SIZE - 1) & PAGE_MASK) + PAGE_SIZE)
	                                                            >> PAGE_SHIFT));

	//mutex_lock(&cb->qp_mutex);
	if (likely(post_inv))
		// no inv from remote, so manually does it on local side //
		ret = ib_post_send(cb->qp, inv_wr, &bad_wr);	// INV+MR //
	else
		ret = ib_post_send(cb->qp, &reg_mr_wr->wr, &bad_wr);	// MR //
	//mutex_unlock(&cb->qp_mutex);
	BUG_ON(ret);

/*
	struct ib_send_wr inv_wr;
	struct ib_reg_wr reg_mr_wr;
	if (mode == RDMA_RKEY_ACT) {
		reg_mr = cb->reg_mr_act;
		//inv_wr = &cb->inv_wr_act;
		reg_mr_wr.wr.opcode = IB_WR_REG_MR;
		reg_mr_wr.mr = cb->reg_mr_act;
		inv_wr.opcode = IB_WR_LOCAL_INV;
		inv_wr.next = &reg_mr_wr.wr;
		//reg_mr_wr = &cb->reg_mr_wr_act;
	} else if (mode == RDMA_RKEY_PASS) {
		reg_mr = cb->reg_mr_pass;
		//inv_wr = &cb->inv_wr_pass;
		reg_mr_wr.wr.opcode = IB_WR_REG_MR;
		reg_mr_wr.mr = cb->reg_mr_pass;
		inv_wr.opcode = IB_WR_LOCAL_INV;
		inv_wr.next = &reg_mr_wr.wr;
		//reg_mr_wr = &cb->reg_mr_wr_pass;
#if CONFIG_FARM2WRITE
	} else if (mode == RDMA_FARM2_RKEY_ACT) {
		reg_mr = cb->reg_FaRM2_mr_act;
		//inv_wr = &cb->inv_FaRM2_wr_act;
		reg_mr_wr.wr.opcode = IB_WR_REG_MR;
		reg_mr_wr.mr = cb->reg_FaRM2_mr_act;
		inv_wr.opcode = IB_WR_LOCAL_INV;
		inv_wr.next = &reg_mr_wr.wr;
		//reg_mr_wr = &cb->reg_FaRM2_mr_wr_act;
	} else if (mode == RDMA_FARM2_RKEY_PASS) {
		reg_mr = cb->reg_FaRM2_mr_pass;
		//inv_wr = &cb->inv_FaRM2_wr_pass;
		reg_mr_wr.wr.opcode = IB_WR_REG_MR;
		reg_mr_wr.mr = cb->reg_FaRM2_mr_pass;
		inv_wr.opcode = IB_WR_LOCAL_INV;
		inv_wr.next = &reg_mr_wr.wr;
		//reg_mr_wr = &cb->reg_FaRM2_mr_wr_pass;
#endif
	}

	inv_wr.ex.invalidate_rkey = reg_mr->rkey;
	ib_update_fast_reg_key(reg_mr, cb->key);
	reg_mr_wr.key = reg_mr->rkey;

	reg_mr_wr.access = IB_ACCESS_REMOTE_READ	|
						IB_ACCESS_REMOTE_WRITE	|
						IB_ACCESS_LOCAL_WRITE	|
						IB_ACCESS_REMOTE_ATOMIC;

	sg_dma_address(&sg) = buf;
	sg_dma_len(&sg) = rdma_len;

	ret = ib_map_mr_sg(reg_mr, &sg, 1, PAGE_SIZE);
			// snyc: use ib_dma_sync_single_for_cpu/dev dev:accessed by IB
	BUG_ON(ret <= 0 || ret > ((((MAX_RDMA_SIZE - 1) & PAGE_MASK) + PAGE_SIZE)
	                                                            >> PAGE_SHIFT));
	if (likely(post_inv))
		// no inv from remote, so manually does it on local side //
		ret = ib_post_send(cb->qp, &inv_wr, &bad_wr);	// INV+MR //
	else
		ret = ib_post_send(cb->qp, &reg_mr_wr.wr, &bad_wr);	// MR //
	BUG_ON(ret);
*/
	return reg_mr->rkey;
}

/*
 * init all buffers < 1.pd->cq->qp 2.[mr] 3.xxx >
 */
static int ib_setup_buffers(struct ib_cb *cb)
{
	int i, ret, page_list_len;
#if CONFIG_FARM2WRITE
	int FaRM2_page_list_len;
	FaRM2_page_list_len =
			(((MAX_RDMA_FARM2_SIZE - 1) & PAGE_MASK) + PAGE_SIZE)
														>> PAGE_SHIFT;
#endif
	page_list_len = (((MAX_RDMA_SIZE - 1) & PAGE_MASK) + PAGE_SIZE)
															>> PAGE_SHIFT;
	for (i = 0; i < MAX_MR_SIZE; i++) {
		/* fill out lkey and rkey */
		cb->reg_mr_act[i] = ib_alloc_mr(cb->pd,
										IB_MR_TYPE_MEM_REG, page_list_len);
		if (IS_ERR(cb->reg_mr_act[i])) {
			ret = PTR_ERR(cb->reg_mr_act[i]);
		goto bail;
		}

		cb->reg_mr_pass[i] = ib_alloc_mr(cb->pd, IB_MR_TYPE_MEM_REG, page_list_len);
		if (IS_ERR(cb->reg_mr_pass[i])) {
			ret = PTR_ERR(cb->reg_mr_pass[i]);
			goto bail;
		}

#if CONFIG_FARM2WRITE
		cb->reg_FaRM2_mr_pass[i] = ib_alloc_mr(cb->pd,
								IB_MR_TYPE_MEM_REG, FaRM2_page_list_len);
		if (IS_ERR(cb->reg_FaRM2_mr_pass[i])) {
			ret = PTR_ERR(cb->reg_FaRM2_mr_pass[i]);
			goto bail;
		}

		cb->FaRM2_buf_pass[i] = kmalloc(FaRM2_PASS_DATA_SIZE, GFP_KERNEL);
		if (!cb->FaRM2_buf_pass[i]) {
			ret = -ENOMEM;
			goto bail;
		}
		cb->FaRM2_dma_addr_pass[i] = dma_map_single(cb->pd->device->dma_device,
					   cb->FaRM2_buf_pass[i], FaRM2_PASS_DATA_SIZE, DMA_BIDIRECTIONAL);
		*cb->FaRM2_buf_pass[i] = 1;
#endif
	}

#if CONFIG_FARM2WRITE
	cb->reg_FaRM2_mr_act = ib_alloc_mr(cb->pd,
						IB_MR_TYPE_MEM_REG, FaRM2_page_list_len);
	if (IS_ERR(cb->reg_FaRM2_mr_act)) {
		ret = PTR_ERR(cb->reg_FaRM2_mr_act);
		goto bail;
	}

	cb->FaRM2_buf_act = kmalloc(FaRM2_ACT_DATA_SIZE, GFP_KERNEL);
	if (!cb->FaRM2_buf_act) {
        ret = -ENOMEM;
        goto bail;
    }
    cb->FaRM2_dma_addr_act = dma_map_single(cb->pd->device->dma_device,
				   cb->FaRM2_buf_act, FaRM2_ACT_DATA_SIZE, DMA_BIDIRECTIONAL);
	for (i = 0; i < FaRM2_ACT_DATA_SIZE; i++)
		*(cb->FaRM2_buf_act + i) = 0;
#endif

	ib_setup_wr(cb);
	return 0;
bail:
	for (i = 0; i < MAX_MR_SIZE; i++) {
		if (cb->reg_mr_act[i] && !IS_ERR(cb->reg_mr_act[i]))
			ib_dereg_mr(cb->reg_mr_act[i]);
		if (cb->reg_mr_pass[i] && !IS_ERR(cb->reg_mr_pass[i]))
			ib_dereg_mr(cb->reg_mr_pass[i]);
#if CONFIG_FARM2WRITE
		if (cb->reg_FaRM2_mr_pass[i] && !IS_ERR(cb->reg_FaRM2_mr_pass[i]))
			ib_dereg_mr(cb->reg_FaRM2_mr_pass[i]);
#endif
	}

#if CONFIG_FARM2WRITE
	if (cb->reg_FaRM2_mr_act && !IS_ERR(cb->reg_FaRM2_mr_act))
		ib_dereg_mr(cb->reg_FaRM2_mr_act);
#endif
	return ret;
}


static int ib_accept(struct ib_cb *cb)
{
	int ret;
	struct rdma_conn_param conn_param;
	
	memset(&conn_param, 0, sizeof conn_param);
	conn_param.responder_resources = 1;
	conn_param.initiator_depth = 1;

	ret = rdma_accept(cb->child_cm_id, &conn_param);
	if (ret) {
		printk(KERN_ERR "rdma_accept error: %d\n", ret);
		return ret;
	}

	wait_event_interruptible(cb->sem, atomic_read(&cb->state) == CONNECTED);

	if (atomic_read(&cb->state) == ERROR) {
		printk(KERN_ERR "wait for CONNECTED state %d\n",
						atomic_read(&cb->state));
		return -1;
	}
	return 0;
}

static void ib_free_buffers(struct ib_cb *cb, u32 mr_ofs)
{
	if (cb->reg_mr_act[mr_ofs])
		ib_dereg_mr(cb->reg_mr_act[mr_ofs]);
	if (cb->reg_mr_pass[mr_ofs])
		ib_dereg_mr(cb->reg_mr_pass[mr_ofs]);
#if CONFIG_FARM2WRITE
	if (cb->reg_FaRM2_mr_act)
		ib_dereg_mr(cb->reg_FaRM2_mr_act);
	if (cb->reg_FaRM2_mr_pass[mr_ofs])
		ib_dereg_mr(cb->reg_FaRM2_mr_pass[mr_ofs]);
#endif
}

static void ib_free_qp(struct ib_cb *cb)
{
	ib_destroy_qp(cb->qp);
	ib_destroy_cq(cb->cq);
	ib_dealloc_pd(cb->pd);
}

static int ib_server_accept(void *arg0)
{
	struct ib_cb *cb = arg0;
	int i, ret = -1;

	ret = ib_setup_qp(cb, cb->child_cm_id);
	if (ret) {
		printk(KERN_ERR "setup_qp failed: %d\n", ret);
		goto err0;
	}

	ret = ib_setup_buffers(cb);
	if (ret) {
		printk(KERN_ERR "ib_setup_buffers failed: %d\n", ret);
		goto err1;
	}
	/* after here, you can send/recv */

	ret = ib_accept(cb);
	if (ret) {
		printk(KERN_ERR "connect error %d\n", ret);
		goto err2;
	}
	return 0;
err2:
	for (i = 0; i < MAX_MR_SIZE; i++)
		ib_free_buffers(cb, i);
err1:
	ib_free_qp(cb);
err0:
	rdma_destroy_id(cb->child_cm_id);
	return ret;
}

static int ib_run_server(void *arg0)
{
	struct ib_cb *my_cb = arg0;
	int ret, i = 0;

	ret = ib_bind_server(my_cb);
	if (ret)
		return ret;

	/* create multiple connections */
	while (1){
		struct ib_cb *target_cb;
		i++;

		if (my_nid+i >= MAX_NUM_NODES)
			break;

		/* Wait for client's Start STAG/TO/Len */
		wait_event_interruptible(my_cb->sem,
					atomic_read(&my_cb->state) == CONNECT_REQUEST);
		if (atomic_read(&my_cb->state) != CONNECT_REQUEST) {
			printk(KERN_ERR "wait for CONNECT_REQUEST state %d\n",
										atomic_read(&my_cb->state));
			continue;
		}
		atomic_set(&my_cb->state, IDLE);

		target_cb = gcb[my_nid+i];
		target_cb->server = 1;

		/* got from INT. Will be used [setup_qp(SRWRirq)] -> setup_buf -> */
		target_cb->child_cm_id = my_cb->child_cm_id;

		if (ib_server_accept(target_cb))
			rdma_disconnect(target_cb->child_cm_id);

		printk("conn_no %d is ready (sever)\n", target_cb->conn_no);
		set_popcorn_node_online(target_cb->conn_no, true);
	}
	return 0;
}

static int ib_bind_client(struct ib_cb *cb)
{
	struct sockaddr_storage sin;
	int ret;

	fill_sockaddr(&sin, cb);

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

	return 0;
}

static int ib_run_client(struct ib_cb *cb)
{
	int i, ret;

	ret = ib_bind_client(cb);
	if (ret)
		return ret;

	ret = ib_setup_qp(cb, cb->cm_id);
	if (ret) {
		printk(KERN_ERR "setup_qp failed: %d\n", ret);
		return ret;
	}

	ret = ib_setup_buffers(cb);
	if (ret) {
		printk(KERN_ERR "ib_setup_buffers failed: %d\n", ret);
		goto err1;
	}

	ret = ib_connect_client(cb);
	if (ret) {
		printk(KERN_ERR "connect error %d\n", ret);
		goto err2;
	}
	return 0;
err2:
	for (i = 0; i < MAX_MR_SIZE; i++)
		ib_free_buffers(cb, i);
err1:
	ib_free_qp(cb);
	return ret;
}


/*
 * IB utility functions for supporting dynamic mapping
 * specifically for RW users such as page migration
 */
u64 __ib_map_act(void *paddr, int conn_no, int rw_size)
{
	struct ib_cb *cb = gcb[conn_no];
	u64 dma_addr = dma_map_single(cb->pd->device->dma_device,
								  paddr, rw_size, DMA_BIDIRECTIONAL);
	pci_unmap_addr_set(cb, rdma_mapping_act, dma_addr);
	return dma_addr;
}

u64 __ib_map_pass(void *paddr, int conn_no, int rw_size)
{
	struct ib_cb *cb = gcb[conn_no];
u64 dma_addr = dma_map_single(cb->pd->device->dma_device,
								  paddr, rw_size, DMA_BIDIRECTIONAL);
	pci_unmap_addr_set(cb, rdma_mapping_pass, dma_addr);
	return dma_addr;
}

/* no need to umap, just remap directly */
void __unmap_act(int conn_no, int rw_size)
{
	struct ib_cb *cb = gcb[conn_no];
	dma_unmap_single(cb->pd->device->dma_device,
					pci_unmap_addr(cb, rdma_mapping_act),
					rw_size, DMA_BIDIRECTIONAL);
}

void __unmap_pass(int conn_no, int rw_size)
{
	struct ib_cb *cb = gcb[conn_no];
	dma_unmap_single(cb->pd->device->dma_device,
					pci_unmap_addr(cb, rdma_mapping_pass),
					rw_size, DMA_BIDIRECTIONAL);
}


/*
 * User doesn't have to take care of concurrency problems.
 * This func will take care of it.
 * User has to free the allocated mem manually
 */
int __ib_kmsg_send(unsigned int dst,
				  struct pcn_kmsg_message *msg,
				  unsigned int msg_size)
{
	int ret;
	struct completion comp;
	struct ib_send_wr *bad_wr;
	struct ib_sge send_sgl = {
			.length = msg_size,
			.lkey = gcb[dst]->pd->local_dma_lkey,
	};
	struct ib_send_wr send_wr = {
			.opcode = IB_WR_SEND,
			.send_flags = IB_SEND_SIGNALED,
			.num_sge = 1,
			.sg_list = &send_sgl,
			.next = NULL,
	};
	u64 send_dma_addr;

	if ( msg_size > PCN_KMSG_MAX_SIZE) {
		printk("%s(): ERROR - MSG %d larger than MAX_MSG_SIZE %ld\n",
					__func__, msg_size, PCN_KMSG_MAX_SIZE);
		BUG();
	}

	if (dst == my_nid) {
		printk(KERN_ERR "No support for sending msg to itself %d\n", dst);
		BUG();
	}

	msg->header.size = msg_size;
	msg->header.from_nid = my_nid;

	init_completion(&comp);
	send_wr.wr_id = (u64)&comp;

	send_dma_addr = dma_map_single(gcb[dst]->pd->device->dma_device,
					msg, msg_size, DMA_BIDIRECTIONAL);
	send_sgl.addr = send_dma_addr;

	ret = ib_post_send(gcb[dst]->qp, &send_wr, &bad_wr);
	BUG_ON(ret);

	atomic_inc(&gcb[dst]->WQ_WR_cnt);
	if (atomic_read(&gcb[dst]->WQ_WR_cnt) >= MAX_SEND_WR)
		BUG();

	wait_for_completion(&comp);

	dma_unmap_single(gcb[dst]->pd->device->dma_device,
					 send_dma_addr, msg->header.size, DMA_BIDIRECTIONAL);
	return 0;
}




/*
 * RDMA READ:
 * send        ----->   irq (recv)
 *                      lock
 *             <=====   perform READ
 *                      unlock
 * irq (recv)  <-----   send
 *
 */
static void handle_remote_thread_rdma_read_request(
						remote_thread_rdma_rw_t *inc_msg, void *target_paddr)
{
	remote_thread_rdma_rw_t *req = (remote_thread_rdma_rw_t*) inc_msg;
	remote_thread_rdma_rw_t *reply;
	struct ib_send_wr *bad_wr;
	int ret, from = req->header.from_nid;
	struct ib_cb *cb = gcb[from];
	struct completion comp;
	u32 mr_ofs;
	int rw_size = req->rdma_header.rw_size;
	u64 dma_addr_pass = dma_map_single(cb->pd->device->dma_device,
				target_paddr, rw_size, DMA_BIDIRECTIONAL);

	struct ib_sge rdma_sgl = {
		.length = rw_size,
		.addr = dma_addr_pass,
	};
	struct ib_rdma_wr rdma_send_wr = {
		.wr.num_sge = 1,
		.wr.sg_list = &rdma_sgl,
		.wr.send_flags = IB_SEND_SIGNALED,
		.rkey = ntohl(req->rdma_header.remote_rkey),
		.remote_addr = ntohll(req->rdma_header.remote_addr),
		.wr.next = NULL,
	};

	init_completion(&comp);
	rdma_send_wr.wr.wr_id = (u64)&comp;

	if (unlikely(cb->read_inv))
		rdma_send_wr.wr.opcode = IB_WR_RDMA_READ_WITH_INV;
	else {
		/* Compose a READ sge with a invalidation */
		rdma_send_wr.wr.opcode = IB_WR_RDMA_READ;
	}

	//mutex_lock(&cb->passive_mutex);
	mr_ofs = get_mr_ofs(cb->conn_no, RDMA_RKEY_PASS);
	rdma_sgl.lkey = ib_rdma_rkey(cb, dma_addr_pass, !cb->read_inv,
								rw_size, mr_ofs, RDMA_RKEY_PASS),

	ret = ib_post_send(cb->qp, &rdma_send_wr.wr, &bad_wr);
	BUG_ON(ret);

	atomic_inc(&cb->WQ_WR_cnt);
	if (atomic_read(&cb->WQ_WR_cnt) >= MAX_SEND_WR)
		BUG();

	wait_for_completion(&comp);

	put_mr_ofs(cb->conn_no, mr_ofs, RDMA_RKEY_PASS);
	//mutex_unlock(&cb->passive_mutex);
	dma_unmap_single(cb->pd->device->dma_device,
					dma_addr_pass, rw_size, DMA_BIDIRECTIONAL);

	reply = pcn_kmsg_alloc_msg(sizeof(*reply));
	BUG_ON(!reply);

	/* ACK */
	reply->header.type = req->rdma_header.rmda_type_res;
	reply->header.prio = PCN_KMSG_PRIO_NORMAL;
	//reply->tgroup_home_cpu = tgroup_home_cpu;
	//reply->tgroup_home_id = tgroup_home_id;

	/* RDMA R/W complete ACK */
	reply->header.is_rdma = true;
	reply->rdma_header.rdma_ack = true;
	reply->rdma_header.is_write = false;
	reply->rdma_header.remote_rkey = ntohl(req->rdma_header.remote_rkey);
	reply->rdma_header.remote_addr = ntohll(req->rdma_header.remote_addr);
	reply->rdma_header.rw_size = rw_size;

	/* for multithreading */
	reply->mr_ofs = req->mr_ofs;
	/* for wait station */
	reply->remote_ws = inc_msg->remote_ws;
	/* for umap dma addr */
	reply->dma_addr_act = inc_msg->dma_addr_act;

	__ib_kmsg_send(req->header.from_nid,
						(struct pcn_kmsg_message*)reply, sizeof(*reply));

	pcn_kmsg_free_msg(reply);
	return;
}

/* FARM implementations will never call this func */
static void handle_remote_thread_rdma_read_response(
										remote_thread_rdma_rw_t *inc_msg)
{
	remote_thread_rdma_rw_t *res = (remote_thread_rdma_rw_t*) inc_msg;
	struct ib_cb *cb = gcb[res->header.from_nid];

	//mutex_unlock(&cb->active_mutex);
	put_mr_ofs(res->header.from_nid, res->mr_ofs, RDMA_RKEY_ACT);

	dma_unmap_single(cb->pd->device->dma_device,
					res->dma_addr_act,
					res->rdma_header.rw_size, DMA_BIDIRECTIONAL);

	/* completed outside is fine by wait station */
	return;
}


/*
 * RDMA WRITE:
 * send        ----->   irq (recv)
 *                      lock
 *             <=====   perform WRITE
 *                      unlock
 * irq (recv)  <-----   send
 *
 *
 * FaRM RDMA WRITE:
 * send        ----->   irq (recv)
 * poll                 lock
 *             <=====   perform WRITE
 *                      unlock
 * done					done
 */
static void handle_remote_thread_rdma_write_request(
					remote_thread_rdma_rw_t *inc_msg, void *target_paddr)
{
	remote_thread_rdma_rw_t *req = (remote_thread_rdma_rw_t*) inc_msg;
#if !CONFIG_FARM_COPY && !CONFIG_FARM2WRITE && !CONFIG_FARM
	remote_thread_rdma_rw_t *reply;
#endif
	int rw_size = req->rdma_header.rw_size;
	struct ib_cb *cb = gcb[req->header.from_nid];
	int ret;
	u64 dma_addr_pass;
	struct completion comp;
	uint32_t remote_pass_len;
	struct ib_send_wr *bad_wr;
	struct ib_sge rdma_sgl;
	struct ib_rdma_wr rdma_send_wr = {
		.wr.sg_list = &rdma_sgl,
		.wr.send_flags = IB_SEND_SIGNALED,
		.wr.opcode = IB_WR_RDMA_WRITE,
		.wr.num_sge = 1,
		.wr.next = NULL,

		.rkey = ntohl(req->rdma_header.remote_rkey),
		.remote_addr = ntohll(req->rdma_header.remote_addr),
	};
	u32 mr_ofs = get_mr_ofs(req->header.from_nid, RDMA_RKEY_PASS);
#if CONFIG_FARM2WRITE
	struct completion comp2;
	//u32 FaRM2_mr_ofs = get_mr_ofs(req->header.from_nid, RDMA_FARM2_RKEY_PASS);
	struct ib_sge rdma_FaRM2_sgl = {
		//.addr = cb->local_FaRM2_laddr[FaRM2_mr_ofs],
		//.lkey = cb->local_FaRM2_lkey[FaRM2_mr_ofs],
		.addr = cb->local_FaRM2_laddr[mr_ofs],
		.lkey = cb->local_FaRM2_lkey[mr_ofs],
		.length = FaRM2_PASS_DATA_SIZE,
	};
	struct ib_rdma_wr rdma_FaRM2_send_wr = {
		.wr.num_sge = 1,
		.wr.sg_list = &rdma_FaRM2_sgl,
		.wr.send_flags = IB_SEND_SIGNALED,
		.wr.opcode = IB_WR_RDMA_WRITE,
		.rkey = cb->remote_FaRM2_rkey,
		.remote_addr = cb->remote_FaRM2_raddr + req->mr_ofs,
		.wr.next = NULL,
	};
#endif
#if CONFIG_FARM_COPY
	char *_FaRM_pass_buf;
#endif

	//mutex_lock(&cb->passive_mutex);

#if CONFIG_FARM_COPY
	 remote_pass_len = rw_size + FaRM_HEAD_AND_TAIL;
#else
	 remote_pass_len = rw_size;
#endif

#if CONFIG_FARM_COPY
	_FaRM_pass_buf = cb->FaRM_pass_buf[mr_ofs];	//FaRM copy use a single buf

	/* FaRM w/ buffer copying - make a new buffer aligned with the formate */
	if (target_paddr && rw_size != 0) {
		/* FaRM head: length + 1B */
		*(_FaRM_pass_buf + 3) = (char)((rw_size) >> 0);
		*(_FaRM_pass_buf + 2) = (char)((rw_size) >> 8);
		*(_FaRM_pass_buf + 1) = (char)((rw_size) >> 16);
		*(_FaRM_pass_buf + 0) = (char)((rw_size) >> 24);
		*(_FaRM_pass_buf + FaRM_HEAD - 1) = FaRM_IS_DATA;
		/* payload */
		memcpy(_FaRM_pass_buf + FaRM_HEAD, target_paddr, rw_size);
		/* FaRM tail: 1B */
		memset(_FaRM_pass_buf + rw_size + FaRM_HEAD_AND_TAIL - 1, 1, 1);
	}
	else
		memset(_FaRM_pass_buf, FaRM_IS_EMPTY, FaRM_HEAD);

	dma_addr_pass = dma_map_single(cb->pd->device->dma_device,
		  _FaRM_pass_buf, rw_size + FaRM_HEAD_AND_TAIL, DMA_BIDIRECTIONAL);
#else
	dma_addr_pass = dma_map_single(cb->pd->device->dma_device,
								  target_paddr, rw_size, DMA_BIDIRECTIONAL);
#endif


	rdma_sgl.addr = dma_addr_pass;
	rdma_sgl.length = remote_pass_len;
	rdma_sgl.lkey = ib_rdma_rkey(cb, dma_addr_pass, 1,
								remote_pass_len, mr_ofs, RDMA_RKEY_PASS);
	/*
	struct ib_send_wr inv;			//- for ib_post_send -//
	rdma_send_wr.wr.next = &inv;	//- followed by a inv -//
	memset(&inv, 0, sizeof inv);
	inv.opcode = IB_WR_LOCAL_INV;
	inv.ex.invalidate_rkey = cb->reg_mr_pass->rkey;
	inv.send_flags = IB_SEND_FENCE;
	*/

	init_completion(&comp);
	rdma_send_wr.wr.wr_id = (u64)&comp;

	ret = ib_post_send(cb->qp, &rdma_send_wr.wr, &bad_wr);
	BUG_ON(ret);

	atomic_inc(&cb->WQ_WR_cnt);
	BUG_ON(atomic_read(&cb->WQ_WR_cnt) >= MAX_SEND_WR);

	wait_for_completion(&comp);

#if CONFIG_FARM_COPY
	dma_unmap_single(cb->pd->device->dma_device,
			dma_addr_pass, rw_size + FaRM_HEAD_AND_TAIL, DMA_BIDIRECTIONAL);
#else
	dma_unmap_single(cb->pd->device->dma_device,
			dma_addr_pass, rw_size, DMA_BIDIRECTIONAL);
#endif

#if CONFIG_FARM2WRITE
	init_completion(&comp2);
	rdma_FaRM2_send_wr.wr.wr_id = (u64)&comp2;
	ret = ib_post_send(cb->qp, &rdma_FaRM2_send_wr.wr, &bad_wr);
	BUG_ON(ret);

	atomic_inc(&cb->WQ_WR_cnt);
	BUG_ON(atomic_read(&cb->WQ_WR_cnt) >= MAX_SEND_WR);

	//printk("remote: req->mr_ofs %u\n", req->mr_ofs);
	wait_for_completion(&comp2);

	//put_mr_ofs(req->header.from_nid, FaRM2_mr_ofs, RDMA_FARM2_RKEY_PASS);
	//put_mr_ofs(req->header.from_nid, mr_ofs, RDMA_RKEY_PASS);
	/* No need to umap FaRM2WRITE polling bits */
#elif !CONFIG_FARM_COPY && !CONFIG_FARM2WRITE && !CONFIG_FARM
	reply = pcn_kmsg_alloc_msg(sizeof(*reply));
	BUG_ON(!reply);

	reply->header.type = req->rdma_header.rmda_type_res;
	reply->header.prio = PCN_KMSG_PRIO_NORMAL;

	/* RDMA W/R complete ACK */
	reply->header.is_rdma = true;
	reply->rdma_header.rdma_ack = true;
	reply->rdma_header.is_write = true;
	reply->rdma_header.remote_rkey = ntohl(req->rdma_header.remote_rkey);
	reply->rdma_header.remote_addr = ntohll(req->rdma_header.remote_addr);
	reply->rdma_header.rw_size = remote_pass_len;

	/* for multi-threading */
	reply->mr_ofs = req->mr_ofs;
	/* for wait station */
	reply->remote_ws = req->remote_ws;
	/* for umap dma addr */
	reply->dma_addr_act = req->dma_addr_act;

	__ib_kmsg_send(req->header.from_nid,
				(struct pcn_kmsg_message*) reply, sizeof(*reply));

	pcn_kmsg_free_msg(reply);
#endif
	//mutex_unlock(&cb->passive_mutex);
	put_mr_ofs(cb->conn_no, mr_ofs, RDMA_RKEY_PASS);
	return;
}

/* FARM implementations will never call this func */
static void handle_remote_thread_rdma_write_response(
										remote_thread_rdma_rw_t *inc_msg)
{
	remote_thread_rdma_rw_t *res = (remote_thread_rdma_rw_t*) inc_msg;
	struct ib_cb *cb = gcb[res->header.from_nid];

	//mutex_unlock(&cb->active_mutex);
	put_mr_ofs(res->header.from_nid, res->mr_ofs, RDMA_RKEY_ACT);

	dma_unmap_single(cb->pd->device->dma_device,
					res->dma_addr_act,
					res->rdma_header.rw_size, DMA_BIDIRECTIONAL);

	/* completed outside is fine by wait station */
	return;
}

/*
 * Caller has to free the msg by him/herself
 * paddr: ptr of pages you wanna perform on RDMA R/W passive side
 */
void handle_rdma_request(remote_thread_rdma_rw_t *inc_msg, void *paddr)
{
	remote_thread_rdma_rw_t *msg = inc_msg;
	if (likely(msg->header.is_rdma)) {
		if(unlikely(inc_msg->rdma_header.rw_size > MAX_RDMA_SIZE)) {
			printk("size %d\n", inc_msg->rdma_header.rw_size);
			BUG();
		}
		if (!msg->rdma_header.rdma_ack) {
			if (msg->rdma_header.is_write)
				handle_remote_thread_rdma_write_request(msg, paddr);
			else
				handle_remote_thread_rdma_read_request(msg, paddr);
		} else {
			if (msg->rdma_header.is_write)
				handle_remote_thread_rdma_write_response(msg);
			else
				handle_remote_thread_rdma_read_response(msg);
		}
	} else {
		printk(KERN_ERR "This is not a rdma request you shouldn't call"
						"\"pcn_kmsg_handle_remote_rdma_request\"\n"
						"from=%u, type=%d, msg_size=%u\n\n",
						msg->header.from_nid,
						msg->header.type,
						msg->header.size);
	}
}

/*
 * Pass msg to upper layer and do the corresponding callback function
 */
static void ib_recv_handler_BottomHalf(struct work_struct *work)
{
	pcn_kmsg_cbftn ftn;
	ib_recv_work_t *w = (ib_recv_work_t *) work;
	struct pcn_kmsg_message *msg = (struct pcn_kmsg_message *)(&w->msg);

	if (unlikely(msg->header.type < 0 ||
				msg->header.type >= PCN_KMSG_TYPE_MAX ||
				msg->header.size < 0 ||
				msg->header.size > PCN_KMSG_MAX_SIZE)) {
		printk(KERN_ERR "Recved invalid msg from %d type %d > MAX %d || "
						"size %d > MAX %lu\n", msg->header.from_nid,
						msg->header.type, PCN_KMSG_TYPE_MAX,
						msg->header.size, PCN_KMSG_MAX_SIZE);
		BUG();
	} else {
		ftn = callbacks[msg->header.type];
		if (ftn != NULL) {
#ifdef CONFIG_POPCORN_STAT
			account_pcn_message_recv(lmsg);
#endif
			ftn((void*)(&((ib_recv_work_t *)work)->msg));
		} else {
			printk(KERN_ERR "Recieved message type %d size %d "
							"has no registered callback!\n",
							msg->header.type, msg->header.size);
			BUG();
		}
	}
	return;
}

/*
 * Parse recved msg in the buf to msg_layer in INT
 */
static int ib_kmsg_recv(struct ib_cb *cb, ib_recv_work_t *rws)
{
	INIT_WORK((struct work_struct *)rws, ib_recv_handler_BottomHalf);
	if (unlikely(!queue_work(msg_handler, (struct work_struct *)rws)))
		BUG();
	return 0;
}

static void ib_cq_event_handler(struct ib_cq *cq, void *ctx)
{
	struct ib_cb *cb = ctx;
	//struct ib_recv_wr *bad_wr;
	int ret;
	struct ib_wc *_wc;	/* work complition->wr_id (work request ID) */
	struct ib_wc wc;	/* work complition->wr_id (work request ID) */

	BUG_ON(cb->cq != cq);
	if (atomic_read(&cb->state) == ERROR) {
		printk(KERN_ERR "< cq completion in ERROR state >\n");
		return;
	}

	ib_req_notify_cq(cb->cq, IB_CQ_NEXT_COMP);
	while ((ret = ib_poll_cq(cb->cq, 1, &wc)) > 0) {
		_wc = &wc;
		if (_wc->status) {
			if (_wc->status == IB_WC_WR_FLUSH_ERR) {
				printk("< cq flushed >\n");
			} else {
				printk(KERN_ERR "< cq completion failed with "
					"wr_id %Lx status %d opcode %d vender_err %x >\n",
					_wc->wr_id, _wc->status, _wc->opcode, _wc->vendor_err);
				BUG_ON(_wc->status);
				goto error;
			}
		}

		switch (_wc->opcode) {
		case IB_WC_SEND:
			atomic_dec(&gcb[cb->conn_no]->WQ_WR_cnt);
			complete((struct completion *)_wc->wr_id);
			break;

		case IB_WC_RECV:
			ret = ib_kmsg_recv(cb, (ib_recv_work_t*)_wc->wr_id);
			BUG_ON(ret);
			break;

		case IB_WC_RDMA_WRITE:
			atomic_dec(&gcb[cb->conn_no]->WQ_WR_cnt);
			complete((struct completion *)_wc->wr_id);
			break;

		case IB_WC_RDMA_READ:
			atomic_dec(&gcb[cb->conn_no]->WQ_WR_cnt);
			complete((struct completion *)_wc->wr_id);
			break;

		case IB_WC_LOCAL_INV:
			printk("IB_WC_LOCAL_INV:\n");
			break;

		case IB_WC_REG_MR:
			printk("IB_WC_REG_MR:\n");
			//complete((struct completion *)_wc->wr_id);
			break;

		default:
			printk(KERN_ERR "< %s:%d Unexpected opcode %d, Shutting down >\n",
							__func__, __LINE__, _wc->opcode);
			goto error;	/* TODO for rmmod */
			//wake_up_interruptible(&cb->sem);
			//ib_req_notify_cq(cb->cq, IB_CQ_NEXT_COMP);
			return;
		}
	}
	return;

error:
	atomic_set(&cb->state, ERROR);
	wake_up_interruptible(&cb->sem);
}


/*
 * Your request must be done by kmalloc()
 * You have to free the buf by yourself
 *
 * rw_size: size you wanna RDMA RW to perform
 *
 * READ/WRITE:
 * if R/W
 * [active lock]
 * send		 ----->   irq (recv)
 *					   |-passive lock R/W
 *					   |-perform R/W
 *					   |-passive unlock R/W
 * irq (recv)   <-----   |-send
 *  |-active unlock
 *
 * FaRM WRITE: user gives last bit for polling
 * [active lock]
 * send		 ----->   irq (recv)
 * 						|-passive lock R/W
 * polling				|-perform WRITE
 *						|-passive unlock R/W
 * active unlock
 *
 * FaRM2WRITE:
 * [active lock]
 * send		 ----->   irq (recv)
 * 						|-passive lock R/W
 *						|-perform WRITE
 *						|-passive unlock R/W
 * polling				|- WRITE (signal)
 * active unlock
 */
char *ib_kmsg_send_rdma(unsigned int dst, remote_thread_rdma_rw_t *msg,
					  unsigned int msg_size, unsigned int rw_size)
{
	u32 mr_ofs;
	uint32_t rkey;
	u64 dma_addr_act;
#if CONFIG_FARM2WRITE || CONFIG_FARM_COPY || CONFIG_FARM
	char *poll_tail_at;
#endif
	struct ib_cb *cb = gcb[dst];
#if CONFIG_FARM_COPY
	char *FaRM_act_buf;
#endif
#if CONFIG_FARM2WRITE
	//u32 FaRM2_poll_ofs;
#endif
	BUG_ON(rw_size <= 0);

	if (!msg->rdma_header.is_write)
		if (!msg->rdma_header.your_buf_ptr)
			BUG();

	if (dst == my_nid) {
		printk(KERN_ERR "No support for sending msg to itself %d\n", dst);
		return 0;
	}

	msg->header.is_rdma = true;
	msg->header.from_nid = my_nid;
	msg->rdma_header.rdma_ack = false;
	msg->rdma_header.rw_size = rw_size;

#if CONFIG_FARM_COPY
	if (msg->rdma_header.is_write) {
		FaRM_act_buf = kzalloc(rw_size + FaRM_HEAD_AND_TAIL, GFP_KERNEL);
		dma_addr_act = dma_map_single(cb->pd->device->dma_device,
						FaRM_act_buf, rw_size + FaRM_HEAD_AND_TAIL,
						DMA_BIDIRECTIONAL);
	} else {
		dma_addr_act = dma_map_single(cb->pd->device->dma_device,
						msg->rdma_header.your_buf_ptr,
						rw_size, DMA_BIDIRECTIONAL);
	}
#else
	dma_addr_act = dma_map_single(cb->pd->device->dma_device,
					msg->rdma_header.your_buf_ptr,
					rw_size, DMA_BIDIRECTIONAL);
#endif

	//mutex_lock(&cb->active_mutex);	/* protects rkey */

	mr_ofs = get_mr_ofs(dst, RDMA_RKEY_ACT);
//printk("origin: mr_ofs %d\n", mr_ofs);
	if (msg->rdma_header.is_write) {
#if CONFIG_FARM_COPY
		rkey = ib_rdma_rkey(cb, dma_addr_act,
				!cb->server_invalidate,
				rw_size + FaRM_HEAD_AND_TAIL, mr_ofs, RDMA_RKEY_ACT);
#else
		rkey = ib_rdma_rkey(cb, dma_addr_act,
				!cb->server_invalidate, rw_size, mr_ofs, RDMA_RKEY_ACT);
#endif
	} else {
		rkey = ib_rdma_rkey(cb, dma_addr_act,
				!cb->server_invalidate, rw_size, mr_ofs, RDMA_RKEY_ACT);
	}

	msg->rdma_header.remote_addr = htonll(dma_addr_act);
	msg->rdma_header.remote_rkey = htonl(rkey);

	msg->mr_ofs = mr_ofs;
	if (msg->rdma_header.is_write) {
#if !CONFIG_FARM && !CONFIG_FARM_COPY && !CONFIG_FARM2WRITE
		/* free when it's done */
		msg->dma_addr_act = dma_addr_act;
		msg->mr_ofs = mr_ofs;
#elif CONFIG_FARM2WRITE
		msg->mr_ofs = mr_ofs;
#endif
	} else {
		/* free when it's done */
		msg->dma_addr_act = dma_addr_act;
	}

#if CONFIG_FARM2WRITE
	poll_tail_at = cb->FaRM2_buf_act + mr_ofs;
	*poll_tail_at = FaRM_IS_IDLE;
#endif

#if CONFIG_FARM
	poll_tail_at = msg->rdma_header.your_buf_ptr + rw_size - 1;
	*poll_tail_at = 0;
#endif

#if CONFIG_FARM_COPY
	*(FaRM_act_buf + FaRM_HEAD - 1) = 0;	//comment out
#endif
	__ib_kmsg_send(dst, (struct pcn_kmsg_message*) msg, msg_size);

	if (msg->rdma_header.is_write) {
#if CONFIG_FARM2WRITE
		while (*poll_tail_at == 0)
			schedule();

		//mutex_unlock(&cb->active_mutex);
		put_mr_ofs(dst, mr_ofs, RDMA_RKEY_ACT);

		dma_unmap_single(cb->pd->device->dma_device,
						dma_addr_act, rw_size, DMA_BIDIRECTIONAL);
#elif CONFIG_FARM_COPY
		/* polling - not done:0  */
		while (*(FaRM_act_buf + FaRM_HEAD - 1) == 0)
			schedule();

		/* check size - if empty (0xff) */
		if(*(FaRM_act_buf + FaRM_HEAD - 1) == FaRM_IS_EMPTY) {
			put_mr_ofs(dst, mr_ofs, RDMA_RKEY_ACT);
			dma_unmap_single(cb->pd->device->dma_device, dma_addr_act,
						rw_size + FaRM_HEAD_AND_TAIL, DMA_BIDIRECTIONAL);
			kfree(FaRM_act_buf);
			return NULL;
		}

		/* poll at tail */
		poll_tail_at = FaRM_act_buf + rw_size + FaRM_HEAD_AND_TAIL - 1;
		while (*poll_tail_at == 0)
			schedule();

		//mutex_unlock(&cb->active_mutex);
		put_mr_ofs(dst, mr_ofs, RDMA_RKEY_ACT);

		dma_unmap_single(cb->pd->device->dma_device, dma_addr_act,
						rw_size + FaRM_HEAD_AND_TAIL, DMA_BIDIRECTIONAL);
		return FaRM_act_buf;
#elif CONFIG_FARM
		//poll_tail_at = msg->rdma_header.your_buf_ptr + rw_size - 1;
		while (*poll_tail_at == 0)
			schedule();

		//mutex_unlock(&cb->active_mutex);
		put_mr_ofs(dst, mr_ofs, RDMA_RKEY_ACT);

		dma_unmap_single(cb->pd->device->dma_device, dma_addr_act,
						rw_size, DMA_BIDIRECTIONAL);
#else
		/* Response handler will complete and free dma_addr_act */
#endif
	}
	return 0;
}

int ib_kmsg_send(unsigned int dst,
					  struct pcn_kmsg_message *msg,
					  unsigned int msg_size)
{
	msg->header.is_rdma = false;
	return __ib_kmsg_send(dst, msg, msg_size);
}

static void ib_kmsg_free_msg(struct pcn_kmsg_message *msg)
{
	if(msg->header.from_nid == my_nid) {
		kfree(msg);
	} else {
		struct ib_recv_wr *bad_wr;
		ib_recv_work_t *rws = container_of(msg, ib_recv_work_t, msg);
		int ret = ib_post_recv(gcb[msg->header.from_nid]->qp,
								rws->recv_wr, &bad_wr);
		BUG_ON(ret && "ib_post_recv failed");
	}
}

#if CONFIG_FARM2WRITE
static void rdma_FaRM2_key_exchange_request(int dst)
{
	struct FaRM2_init_req_t *req = kmalloc(sizeof(*req), GFP_KERNEL);
	struct ib_cb *cb = gcb[dst];
	u32 rkey;

	req->header.type = PCN_KMSG_TYPE_RDMA_FARM2_KEY_EXCH_REQUEST;
	req->header.prio = PCN_KMSG_PRIO_NORMAL;
	rkey = ib_rdma_rkey(cb, cb->FaRM2_dma_addr_act,
						!cb->server_invalidate,
						FaRM2_ACT_DATA_SIZE, 0, RDMA_FARM2_RKEY_ACT);
	req->remote_addr = htonll(cb->FaRM2_dma_addr_act);
	req->remote_rkey = htonl(rkey);
	ib_kmsg_send(dst, (void*)req, sizeof(*req));
	pcn_kmsg_free_msg(req);
}

static void handle_rdma_FaRM2_key_exchange_request(
									struct pcn_kmsg_message *inc_msg)
{
	struct FaRM2_init_req_t *req = (struct FaRM2_init_req_t*) inc_msg;
	struct ib_cb *cb = gcb[req->header.from_nid];
	int i;

	/* remote info: */
	cb->remote_FaRM2_rkey = ntohl(req->remote_rkey);
	cb->remote_FaRM2_raddr = ntohll(req->remote_addr);
	//cb->remote_FaRM2_rlen = FaRM2_ACT_DATA_SIZE;

	/* local info: */
	//cb->local_FaRM2_llen = FaRM2_PASS_DATA_SIZE;
	for (i = 0; i < MAX_MR_SIZE; i++) {
		cb->local_FaRM2_laddr[i] = cb->FaRM2_dma_addr_pass[i];
		cb->local_FaRM2_lkey[i] = ib_rdma_rkey(cb,
								cb->FaRM2_dma_addr_pass[i], !cb->read_inv,
								FaRM2_PASS_DATA_SIZE, i, RDMA_FARM2_RKEY_PASS);
	}
	pcn_kmsg_free_msg(inc_msg);
}
#endif

/* Initialize callback table to null, set up control and data channels */
int __init initialize()
{
	int i, err, conn_no;
#if CONFIG_FARM_COPY
	int j;
#endif
	msg_layer = "IB";

	printk("- Popcorn Messaging Layer IB Initialization Start -\n");
	/* Establish node numbers according to its IP */
	if (!init_ip_table()) {
		printk("%s(): check your IP table!\n", __func__);
		return -EINVAL;
	}
	/* Create workers for bottom-halves */
	msg_handler = alloc_workqueue("MSGHandBotm", WQ_MEM_RECLAIM, 0);

#if CONFIG_FARM2WRITE
	pcn_kmsg_register_callback(PCN_KMSG_TYPE_RDMA_FARM2_KEY_EXCH_REQUEST,
					(pcn_kmsg_cbftn)handle_rdma_FaRM2_key_exchange_request);
#endif

	send_callback = (send_cbftn)ib_kmsg_send;
	send_rdma_callback = (send_rdma_cbftn)ib_kmsg_send_rdma;
	handle_rdma_callback = (handle_rdma_request_ftn)handle_rdma_request;
	kmsg_free_callback = (kmsg_free_ftn)ib_kmsg_free_msg;

	/* Initilaize the IB: Each node has a connection table like tihs
	 * -------------------------------------------------------------------
	 * | connect | (many)... | my_nid(one) | accept | accept | (many)... |
	 * -------------------------------------------------------------------
	 * my_nid:  no need to talk to itself
	 * connect: connecting to existing nodes
	 * accept:  waiting for the connection requests from later nodes
	 */
	for (i = 0; i < MAX_NUM_NODES; i++) {
		/* Create global Control Block context for each connection */
		gcb[i] = kzalloc(sizeof(struct ib_cb), GFP_KERNEL);

		/* Settup node number */
		conn_no = i;
		gcb[i]->conn_no = i;
		set_popcorn_node_online(i, false);

		/* Setup locks */
		mutex_init(&gcb[i]->send_mutex);
		mutex_init(&gcb[i]->active_mutex);
		mutex_init(&gcb[i]->passive_mutex);
		mutex_init(&gcb[i]->qp_mutex);

		/* Init common parameters */
		gcb[i]->state.counter = IDLE;
		gcb[i]->send_state.counter = IDLE;
		gcb[i]->read_state.counter = IDLE;
		gcb[i]->write_state.counter = IDLE;
		gcb[i]->WQ_WR_cnt.counter = 0;

		/* set up IPv4 address */
		gcb[i]->addr_str = ip_addresses[conn_no];
		in4_pton(ip_addresses[conn_no], -1, gcb[i]->addr, -1, NULL);
		gcb[i]->addr_type = AF_INET;		/* [IPv4]/v6 */

		/* register event handler */
		gcb[i]->cm_id = rdma_create_id(&init_net,
				ib_cma_event_handler, gcb[i], RDMA_PS_TCP, IB_QPT_RC);
		if (IS_ERR(gcb[i]->cm_id)) {
			err = PTR_ERR(gcb[i]->cm_id);
			printk(KERN_ERR "rdma_create_id error %d\n", err);
			goto out;
		}

		gcb[i]->key = i;
		gcb[i]->server = -1;
		gcb[i]->read_inv = 0;
		gcb[i]->server_invalidate = 0;
		init_waitqueue_head(&gcb[i]->sem);

#if CONFIG_FARM_COPY
		/* passive RW buffer */
		for (j = 0; j < MAX_MR_SIZE; j++) {
			gcb[i]->FaRM_pass_buf[j] = kzalloc(MAX_RDMA_SIZE, GFP_KERNEL);
			BUG_ON(!gcb[i]->FaRM_pass_buf[j]);
		}
#endif
	}

	/* Establish connections
	 * Each node has a connection table like tihs:
	 * -------------------------------------------------------------------
	 * | connect | (many)... | my_nid(one) | accept | accept | (many)... |
	 * -------------------------------------------------------------------
	 * my_nid:  no need to talk to itself
	 * connect: connecting to existing nodes
	 * accept:  waiting for the connection requests from later nodes
	 */
	set_popcorn_node_online(my_nid, true);

	/* case 1: [<my_nid: connect] | =my_nid | >=my_nid: accept */
	for (i = 0; i < MAX_NUM_NODES; i++) {
		if (i == my_nid)
			continue;

		conn_no = i;
		if (conn_no < my_nid) {
			/* [connect] | my_nid | accept */
			gcb[conn_no]->server = 0;

			/* server/client dependant init */
			if (ib_run_client(gcb[conn_no])) {
				printk("WRONG!!\n");
				rdma_disconnect(gcb[conn_no]->cm_id);
				return err;
			}

			set_popcorn_node_online(conn_no, true);
			smp_mb();
			printk("conn_no %d is ready (client)\n", conn_no);
		}
	}

	/* case 2: <my_nid: connect | =my_nid | [>=my_nid: accept] */
	ib_run_server(gcb[my_nid]);

	for (i = 0; i < MAX_NUM_NODES; i++) {
		atomic_set(&gcb[i]->state, IDLE);
		atomic_set(&gcb[i]->send_state, IDLE);
		atomic_set(&gcb[i]->read_state, IDLE);
		atomic_set(&gcb[i]->write_state, IDLE);
		atomic_set(&gcb[i]->WQ_WR_cnt, 0);
	}

	for (i = 0;i < MAX_NUM_NODES; i++) {
		while ( !get_popcorn_node_online(i) ) {
			printk("waiting for get_popcorn_node_online(%d)\n", i);
			msleep(3000);
			//TODO: do semephor up&down
		}
	}
	smp_mb();

	printk("------------------------------------------\n");
	printk("- Popcorn Messaging Layer IB Initialized -\n");
	printk("------------------------------------------\n"
										"\n\n\n\n\n\n\n");

	/* Popcorn exchanging arch info */
	for (i = 0; i < MAX_NUM_NODES; i++) {
		if (i == my_nid)
			continue;
		notify_my_node_info(i);
	}

#if CONFIG_FARM2WRITE
	for (i = 0; i < MAX_NUM_NODES; i++) {
		if (i == my_nid)
			continue;
		rdma_FaRM2_key_exchange_request(i);
		//TODO: waiting by wait_station
	}
#endif

	return 0;

out:
	for (i = 0; i < MAX_NUM_NODES; i++){
		if (atomic_read(&gcb[i]->state)) {
			kfree(gcb[i]);
			/* TODO: cut connections */
		}
	}
	return err;
}


/*
 *  Not yet done.
 */
static void __exit unload(void)
{
	int i, j;
	printk("TODO: Stop kernel threads\n");

	printk("Release general\n");
	for (i = 0; i < MAX_NUM_NODES; i++) {
		/* mutex */
		mutex_destroy(&gcb[i]->send_mutex);

		/* IB FaRM WRITE passive buffer */
#if CONFIG_FARM_COPY
		for (j = 0; j < MAX_MR_SIZE; j++) {
			if (gcb[i]->FaRM_pass_buf[j])
				kfree(gcb[i]->FaRM_pass_buf[j]);
		}
#endif
	}

	printk("Release IB recv pre-post buffers and flush it\n");
	for (i = 0; i < MAX_NUM_NODES; i++) {
	}

	/* TODO: test rdma_disconnect() */
	printk("rdma_disconnect() only on one side\n");
	for (i = 0; i < MAX_NUM_NODES; i++) {
		if (i == my_nid)
			continue;
		if (i < my_nid) {
			/* client */
			if (gcb[i]->cm_id)
				//if (rdma_disconnect(gcb[i]->cm_id))
				//	BUG();
				;
		} else {
			/* server */
			if (gcb[i]->child_cm_id)
				if (rdma_disconnect(gcb[i]->child_cm_id))
					BUG();
		}
		//if (gcb[i]->cm_id)
		//	rdma_disconnect(gcb[i]->cm_id);
	}

	printk("Release IB server/client productions \n");
	for (i = 0; i < MAX_NUM_NODES; i++) {
		struct ib_cb *cb = gcb[i];

		if (!get_popcorn_node_online(i))
			continue;

		set_popcorn_node_online(i, false);

		if (i == my_nid)
			continue;

		if (i < my_nid) {
			/* client */
			for (j = 0; j < MAX_MR_SIZE; j++)
				ib_free_buffers(cb, j);
			ib_free_qp(cb);
		} else {
			/* server */
			for (j = 0; j < MAX_MR_SIZE; j++)
				ib_free_buffers(cb, j);
			ib_free_qp(cb);
			rdma_destroy_id(cb->child_cm_id);
		}
	}

#if CONFIG_FARM2WRITE
	printk("Release RDMA relavant\n");
	for (i = 0; i < MAX_NUM_NODES; i++) {
		kfree(gcb[i]->FaRM2_buf_act);
		kfree(gcb[i]->FaRM2_buf_pass);
		for (j = 0; j < MAX_MR_SIZE; j++) {
			dma_unmap_single(gcb[i]->pd->device->dma_device,
							gcb[i]->FaRM2_dma_addr_pass[j],
							FaRM2_PASS_DATA_SIZE, DMA_BIDIRECTIONAL);
		}
		dma_unmap_single(gcb[i]->pd->device->dma_device,
						gcb[i]->FaRM2_dma_addr_act,
						FaRM2_ACT_DATA_SIZE, DMA_BIDIRECTIONAL);
	}
#endif

	printk("Release cb context\n");
	for (i = 0; i < MAX_NUM_NODES; i++) {
		kfree(gcb[i]);
	}

	printk("Successfully unloaded module!\n");
}

module_init(initialize);
module_exit(unload);
MODULE_LICENSE("GPL");
