/*
 * pcn_kmesg.c - Kernel Module for Popcorn Messaging Layer over Socket
 * Author: Jack Chuang
 *		  NOT YET READY TO BE MERGED!!!!!!!!!!!
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

/* Jack
 *  mssg layer multi-version
 *  msg sent to data_sock[conn_no] according to dest_cpu
 * 	Note:
 * 			multithread send/recv (test4) works w/o mutex_lock(&_cb->qp_mutex);
 * 			multithread READ (test5) works w/o mutex_lock(&_cb->qp_mutex);
 * 			multithread WRITE (test6) works w/o mutex_lock(&_cb->qp_mutex);
 * 			multithread all together DOESN'T work w/o mutex_lock(&_cb->qp_mutex);
 *  TODO:
 *			test htonll and remove #define htonll(x) cpu_to_be64((x))
 *			test ntohll and remove #define ntohll(x) cpu_to_be64((x))
 *			cb -> _cb
 * 			make parameters in krping_create_qp global
 *
 */
#define SMART_IB_MSG 0

#define POPCORN_DEBUG_MSG_IB 0
#if POPCORN_DEBUG_MSG_IB
#define EXP_LOG(...) printk(__VA_ARGS__)
#define MSG_RDMA_PRK(...) printk(__VA_ARGS__)
#define KRPRINT_INIT(...) printk(__VA_ARGS__)
#define MSG_SYNC_PRK(...) printk(__VA_ARGS__)
#define DEBUG_LOG(...) printk(__VA_ARGS__)

#else
#define EXP_LOG(...)
#define MSG_RDMA_PRK(...)
#define KRPRINT_INIT(...)
#define MSG_SYNC_PRK(...)
#define DEBUG_LOG(...)
#endif 

#define EXP_DATA(...) printk(__VA_ARGS__)


#define htonll(x) cpu_to_be64((x))
#define ntohll(x) cpu_to_be64((x)) 

char net_dev_name[]="ib0";
const char net_dev_name2[]="p7p1"; //another special case, Xgene(ARM)

char* ip_table[] = { "192.168.69.127",		// echo3 ib0
					 "192.168.69.128",		// echo4 ib0
					 "192.168.69.129"};		// none ib0
// temporary solution................
uint32_t ip_table2[] = { (192<<24 | 168<<16 | 69<<8 | 127),		// echo3 ib0
						 (192<<24 | 168<<16 | 69<<8 | 128),		// echo4 ib0
						 (192<<24 | 168<<16 | 69<<8 | 129)};	// none ib0

/* dbg */
atomic_t g_rw_ticket;	// dbg, from1
atomic_t g_send_ticket;  // dbg, from1
atomic_t g_recv_ticket;  // dbg, from1

#define PORT 1000
#define MAX_RDMA_SIZE 4*1024*1024 // MAX R/W BUFFER SIZE
/*
 * HW info:
 * attr.cqe = cb->txdepth * 8;
 * - cq entries - indicating we want room for ten entries on the queue.
 *   This number should be set large enough that the queue isn’t overrun.
 */
//recv
#define MAX_RECV_WR 15000	// important!! If only sender crash, must check it.
//send
#define RPING_SQ_DEPTH 128	// sender depth (txdepth)
#define SEND_DEPTH 8
//[x.xxxxxx] mlx5_core 0000:01:00.0: swiotlb buffer is full (sz: 8388608 bytes)

#define RECV_WQ_THRESHOLD 10
#define INT_MASK 0
//#define INT_MASK (int)((int)0-1)

#define IDLE 1
#define CONNECT_REQUEST 2
#define ADDR_RESOLVED 3
#define ROUTE_RESOLVED 4
#define CONNECTED 5
#define RDMA_READ_ADV 6
#define RDMA_READ_COMPLETE 7
#define RDMA_WRITE_ADV 8
#define RDMA_WRITE_COMPLETE 9
#define RDMA_SEND_COMPLETE 10
#define RDMA_RECV_COMPLETE 11
#define RDMA_SEND_NOT_COMPLETE 12
#define BUSY 13
#define ERROR 14

LIST_HEAD(krping_cbs);
DEFINE_MUTEX(krping_mutex);

/* ib configurations */
int g_conn_responder_resuorces = 1;
int g_conn_initiator_depth = 1;
int g_conn_retry_count = 10;

struct krping_stats {
	atomic_t send_msgs;
	atomic_t recv_msgs;
	atomic_t write_msgs;
	atomic_t read_msgs;
};

/*
 * rq_wr -> wc
 */
struct wc_struct {
	struct pcn_kmsg_long_message *element_addr;
	struct ib_sge *recv_sgl;
	struct ib_recv_wr *rq_wr;
};

/*
 * Control block struct.
 */
struct krping_cb {
	int server;		 /* 0 iff client */
	struct ib_cq *cq;   // can split into two send/recv
	struct ib_pd *pd;
	struct ib_qp *qp;
	struct ib_mr *dma_mr;

	struct ib_fast_reg_page_list *page_list;
	int page_list_len;
	struct ib_reg_wr reg_mr_wr;
	struct ib_reg_wr reg_mr_wr_passive;
	struct ib_send_wr invalidate_wr;
	struct ib_send_wr invalidate_wr_passive;
	struct ib_mr *reg_mr;
	struct ib_mr *reg_mr_passive;
	int server_invalidate;
	int recv_size;
	int read_inv;
	u8 key;

	/* has been changed to be dynamically allocated */
	//u64 recv_dma_addr;
	////DECLARE_PCI_UNMAP_ADDR(recv_mapping)	// cannot compile
	//u64 recv_mapping;

	struct ib_send_wr sq_wr;				/* send work requrest record */
	struct ib_sge send_sgl;
	struct pcn_kmsg_long_message send_buf;	/* single send buf */ /* msg unit */
	u64 send_dma_addr;
	//DECLARE_PCI_UNMAP_ADDR(send_mapping)	// cannot compile
	u64 send_mapping;

	struct ib_rdma_wr rdma_sq_wr;	/* rdma work request record */
	struct ib_sge rdma_sgl;			/* rdma single SGE */

	/* a rdma buf for active */
	char *rw_active_buf;		 /* used as rdma sink */
	u64  active_dma_addr;	 /* for active buffer */
	//DECLARE_PCI_UNMAP_ADDR(rdma_mapping) // cannot compile
	u64 rdma_mapping;
	struct ib_mr *rdma_mr;

	uint32_t remote_rkey;		/* save remote RKEY */
	uint64_t remote_addr;		/* save remote TO */
	uint32_t remote_len;		/* save remote LEN */

	/* a rdma buf for passive */
	char *rw_passive_buf;			/* passive R/W buffer */
	u64  passive_dma_addr;		/* passive R/W buffer addr */
	//DECLARE_PCI_UNMAP_ADDR(start_mapping) // cannot compile
	u64 start_mapping;
	struct ib_mr *start_mr;
	
	atomic_t state;				/* used for cond/signalling */
	atomic_t send_state;
	atomic_t recv_state;
	atomic_t read_state;
	atomic_t write_state;
	wait_queue_head_t sem;
	struct krping_stats stats;

	uint16_t port;				/* dst port in NBO */
	u8 addr[16];				/* dst addr in NBO */
	char *addr_str;				/* dst addr string */
	uint8_t addr_type;			/* ADDR_FAMILY - IPv4/V6 */
	int txdepth;				/* SQ depth */
	unsigned long rdma_size;	/* ping data size */
	
	/* not used */
	int verbose;				/* verbose logging */
	int count;					/* ping count */
	int validate;				/* validate ping data */
	int wlat;					/* run wlat test */
	int rlat;					/* run rlat test */
	int bw;						/* run bw test */
	int duplex;					/* run bw full duplex test */
	int poll;					/* poll or block for rlat test */
	int local_dma_lkey;			/* use 0 for lkey */
	int frtest;					/* reg test */

	/* CM stuff */
	struct rdma_cm_id *cm_id;		/* connection on client side */
									/* listener on server side */
	struct rdma_cm_id *child_cm_id;	/* connection on server side */
	struct list_head list;
	int conn_no;

	/* sync */
	struct mutex send_mutex;
	struct mutex recv_mutex;
	struct mutex active_mutex;
	struct mutex passive_mutex;	/* passive lock*/
	struct mutex qp_mutex;		/* protect ib_post_send(qp) */
	atomic_t active_cnt;		/* used for cond/signalling */
	atomic_t passive_cnt;		/* used for cond/signalling */

	/* for sync dbg */
	spinlock_t rw_slock;
	atomic_t g_all_ticket;
	atomic_t g_now_ticket;
	/* for dbg */
	atomic_t g_wr_id;			/*  for assigning wr_id */
};

/* utilities */
u32 krping_rdma_rkey(struct krping_cb *cb, u64 buf, 
									int post_inv, int rdma_len);
u32 krping_rdma_rkey_passive(struct krping_cb *cb, u64 buf, 
									int post_inv,int rdma_len);

/*
 * List of running krping threads. 
 */
struct krping_cb *cb[MAX_NUM_NODES];
EXPORT_SYMBOL(cb);
struct krping_cb *cb_listen;

int ib_kmsg_send_long(unsigned int dest_cpu,
						struct pcn_kmsg_long_message *lmsg,
						unsigned int msg_size);
int ib_kmsg_send_rdma(unsigned int dest_cpu,
						struct pcn_kmsg_long_message *lmsg,
						unsigned int rw_size);
int ib_kmsg_send_smart(unsigned int dest_cpu,
						struct pcn_kmsg_long_message *lmsg,
						unsigned int msg_size);

/* Popcorn utility */
uint32_t get_host_ip(char **tmp_net_dev_name);
static int __init initialize(void);
extern pcn_kmsg_cbftn callbacks[PCN_KMSG_TYPE_MAX];
extern send_cbftn send_callback;
extern send_cbftn send_callback_rdma;

/* funcs */
static int krping_create_qp(struct krping_cb *cb);
static int ib_kmsg_recv_long(struct krping_cb *cb, struct wc_struct *wcs);

/* workqueue */
struct workqueue_struct *msg_handler;

/* workqueue arg */
typedef struct {
	struct work_struct work;
	struct pcn_kmsg_long_message lmsg;
} pcn_kmsg_work_t;

static int krping_cma_event_handler(struct rdma_cm_id *cma_id,
										struct rdma_cm_event *event)
{
	int ret;
	struct krping_cb *_cb = cma_id->context; // !! use cm_id to retrive cb
	static int cma_event_cnt = 0;
	MSGPRINTK("[[[[[external]]]]] conn_no %d (%s) >>>>>>>> %s(): "
			  "cma_event type %d cma_id %p (%s)\n", _cb->conn_no,
			(my_nid == _cb->conn_no) ? "server" : "client", __func__,
			event->event, cma_id, (cma_id == _cb->cm_id) ? "parent" : "child");
	MSGPRINTK("< cma_id %p _cb->cm_id %p >\n", cma_id, _cb->cm_id);

	switch (event->event) {
	case RDMA_CM_EVENT_ADDR_RESOLVED:
		MSGPRINTK("< ------------RDMA_CM_EVENT_ADDR_RESOLVED------------ >\n");
		//_cb->state = ADDR_RESOLVED;
		atomic_set(&_cb->state, ADDR_RESOLVED);
		ret = rdma_resolve_route(cma_id, 2000);
		if (ret) {
			printk(KERN_ERR "< rdma_resolve_route error %d >\n", ret);
			wake_up_interruptible(&_cb->sem);
		}
		break;

	case RDMA_CM_EVENT_ROUTE_RESOLVED:
		//_cb->state = ROUTE_RESOLVED;
		atomic_set(&_cb->state, ROUTE_RESOLVED);
		wake_up_interruptible(&_cb->sem);
		break;

	case RDMA_CM_EVENT_CONNECT_REQUEST:
		//_cb->state = CONNECT_REQUEST;
		atomic_set(&_cb->state, CONNECT_REQUEST);
		MSGPRINTK("< -----CONNECT_REQUEST-----: _cb->child_cm_id %p = "
									"cma_id(external) >\n", _cb->child_cm_id);
		_cb->child_cm_id = cma_id; // distributed to other connections
		MSGPRINTK("< -----CONNECT_REQUEST-----: _cb->child_cm_id %p = "
									"cma_id(external) >\n", _cb->child_cm_id);
		wake_up_interruptible(&_cb->sem);
		break;

	case RDMA_CM_EVENT_ESTABLISHED:
		MSGPRINTK("< -------------CONNECTION ESTABLISHED---------------- >\n");
		atomic_set(&_cb->state, CONNECTED);

		if(cb[my_nid]->conn_no == _cb->conn_no){
			cma_event_cnt++;
			MSGPRINTK("< my business >\n");
			MSGPRINTK("< cb[my_nid]->conn_no %d _cb->conn_no %d "
						"cma_event_cnt %d >\n", cb[my_nid]->conn_no, 
										_cb->conn_no, cma_event_cnt);
		}
		else{
			MSGPRINTK("< none of my business >\n");
			MSGPRINTK("< cb[my_nid]->conn_no %d _cb->conn_no %d "
						"cma_event_cnt %d >\n", cb[my_nid]->conn_no, 
										_cb->conn_no, cma_event_cnt);
		}
		set_popcorn_node_online(my_nid + cma_event_cnt);
		MSGPRINTK("< %s(): _cb->state %d, CONNECTED %d >\n",
						__func__, (int)atomic_read(&(_cb->state)), CONNECTED);
		//wake_up(&_cb->sem); // TODO: test: change back, see if it runs as well
		wake_up_interruptible(&_cb->sem); // default:
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
		MSGPRINTK("< %s(): _cb->state = %d, CONNECTED=%d >\n",
						__func__, (int)atomic_read(&(_cb->state)), CONNECTED);
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
 * Attention: can be in INT
 * Create a recv_sql/rq_we
 */
struct ib_recv_wr* create_recv_wr(int conn_no, bool is_int)
{
	struct krping_cb *_cb = cb[conn_no];
	struct pcn_kmsg_long_message *element_addr;
	struct ib_sge *_recv_sgl;
	struct ib_recv_wr *_rq_wr;
	struct wc_struct *wcs;
	u64 element_dma_addr;

	if(likely(is_int))
		element_addr = kmalloc(sizeof(*element_addr), GFP_ATOMIC);
	else
		element_addr = kmalloc(sizeof(*element_addr), GFP_KERNEL);

	if (!element_addr) {
		printk(KERN_ERR "recv_buf malloc failed\n");
		BUG();
	}

	if(likely(is_int))
		_recv_sgl = kmalloc(sizeof(*_recv_sgl), GFP_ATOMIC);
	else
		_recv_sgl = kmalloc(sizeof(*_recv_sgl), GFP_KERNEL);
	if (!_recv_sgl) {
		printk(KERN_ERR "sgl recv_buf malloc failed\n");
		BUG();
	}

	if(likely(is_int))
		_rq_wr =  kmalloc(sizeof(*_rq_wr), GFP_ATOMIC);
	else
		_rq_wr =  kmalloc(sizeof(*_rq_wr), GFP_KERNEL);
	if (!_rq_wr) {
		printk(KERN_ERR "rq_wr recv_buf malloc failed\n");
		BUG();
	}

	if(likely(is_int))
		wcs = kmalloc(sizeof(*wcs), GFP_ATOMIC);
	else
		wcs = kmalloc(sizeof(*wcs), GFP_KERNEL);
	if (!wcs) {
		printk(KERN_ERR "wcs malloc failed\n");
		BUG();
	}

	// map buf to ib addr space
	element_dma_addr = dma_map_single(_cb->pd->device->dma_device,
									  element_addr, _cb->recv_size,
												DMA_BIDIRECTIONAL);

	// set up sgl
	_recv_sgl->length = _cb->recv_size;
	_recv_sgl->addr = element_dma_addr;
	_recv_sgl->lkey = _cb->pd->local_dma_lkey;

	// set up rq_wr
	_rq_wr->sg_list = _recv_sgl;
	_rq_wr->num_sge = 1;
	_rq_wr->wr_id = (u64)wcs;
	_rq_wr->next = NULL;

	// save all address to release
	wcs->element_addr = element_addr;
	wcs->recv_sgl = _recv_sgl;
	wcs->rq_wr = _rq_wr;

	MSGDPRINTK("_rq_wr %p _cb->recv_size %d element_addr %p\n",
					(void*)_rq_wr, _cb->recv_size, (void*)element_addr);
	return _rq_wr;
}

static void krping_cq_event_handler(struct ib_cq *cq, void *ctx)
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
	if (atomic_read(&(_cb->state)) == ERROR) {
		printk(KERN_ERR "< cq completion in ERROR state >\n");
		return;
	}

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
			atomic_inc(&_cb->stats.send_msgs);
			DEBUG_LOG("<<< --- from %d [[[[[ SEND ]]]]] COMPLETION %d --- >>>\n",
							_cb->conn_no, atomic_read(&_cb->stats.send_msgs));
			//cb->stats.send_bytes += cb->send_sgl.length;
			atomic_set(&_cb->state, RDMA_SEND_COMPLETE);
			wake_up_interruptible(&_cb->sem);
			break;

		case IB_WC_RDMA_WRITE:
			atomic_inc(&_cb->stats.write_msgs);
			DEBUG_LOG("<<<<< ----- from %d [[[[[ RDMA WRITE ]]]]] "
							"COMPLETION %d ----- (good) >>>>>\n",
							_cb->conn_no, atomic_read(&_cb->stats.write_msgs));
			atomic_set(&_cb->write_state, RDMA_WRITE_COMPLETE);
			wake_up_interruptible(&_cb->sem);
			break;

		case IB_WC_RDMA_READ:
			atomic_inc(&_cb->stats.read_msgs);
			DEBUG_LOG("<<<<< ----- from %d [[[[[ RDMA READ ]]]]] "
							"COMPLETION %d ----- (good) >>>>>\n",
							_cb->conn_no, atomic_read(&_cb->stats.read_msgs));
			atomic_set(&_cb->read_state, RDMA_READ_COMPLETE);
			wake_up_interruptible(&_cb->sem);
			break;

		case IB_WC_RECV:
			recv_cnt++;
			MSG_RDMA_PRK("Jack: ret %d recv_cnt %d\n", ret, recv_cnt);
			atomic_inc(&_cb->stats.recv_msgs);

			DEBUG_LOG("<<< --- from %d [[[[[ RECV ]]]]] COMPLETION %d --- >>>\n",
							  _cb->conn_no, atomic_read(&_cb->stats.recv_msgs));

#ifdef CONFIG_POPCORN_DEBUG_MSG_LAYER_VERBOSE
#if 0
			// you don't know whether is a rdma request or not
			MSG_RDMA_PRK("< info > _wc->wr_id %p rw_t %d "
						 "r_recv_ticket %lu r_rdma_ticket %d rdma_ack \"%s\"\n",
						 (void*)_wc->wr_id,
			 			 ((struct wc_struct*)_wc->wr_id)->element_addr->
			 												rw_ticket,
						 ((struct wc_struct*)_wc->wr_id)->element_addr->
			 												ticket,
						 ((struct wc_struct*)_wc->wr_id)->element_addr->
			 												rdma_ticket,
						 ((struct wc_struct*)_wc->wr_id)->element_addr->
			 									rdma_ack?"true":"false");
#endif
#endif

			ret = ib_kmsg_recv_long(_cb, (struct wc_struct*)_wc->wr_id);
			if (ret) {
				printk(KERN_ERR "< recv wc error: %d >\n", ret);
				goto error;
			}
			break;

		default:
			printk(KERN_ERR "< %s:%d Unexpected opcode %d, Shutting down >\n",
											__func__, __LINE__, _wc->opcode);
			goto error;
		}

		if(recv_cnt >= RECV_WQ_THRESHOLD)
			break;
	}

	for(i=0; i<recv_cnt; i++) {
		struct ib_recv_wr *_rq_wr = create_recv_wr(_cb->conn_no, true);
		ret = ib_post_recv(_cb->qp, _rq_wr, &bad_wr);
		if (ret) {
			printk(KERN_ERR "ib_post_recv failed: %d\n", ret);
			BUG();
		}
	}

	MSGPRINTK("\n[[[[[external done]]]]] node %d\n\n", _cb->conn_no);
	ib_req_notify_cq(_cb->cq, IB_CQ_NEXT_COMP);
	return;
error:
	// TODO: cleanup
	if(_wc->opcode==0)
		atomic_set(&_cb->state, RDMA_SEND_NOT_COMPLETE);
	else
		atomic_set(&_cb->state, ERROR);
	wake_up_interruptible(&_cb->sem);
}

static int krping_connect_client(struct krping_cb *cb)
{
	struct rdma_conn_param conn_param;
	int ret;

	MSGPRINTK("\n->%s();\n", __func__);

	memset(&conn_param, 0, sizeof conn_param);
	conn_param.responder_resources = g_conn_responder_resuorces;
	conn_param.initiator_depth = g_conn_initiator_depth;
	conn_param.retry_count = g_conn_retry_count;

	ret = rdma_connect(cb->cm_id, &conn_param);
	if (ret) {
		printk(KERN_ERR "rdma_connect error %d\n", ret);
		return ret;
	}

	wait_event_interruptible(cb->sem,
						(int)atomic_read(&(cb->state)) == CONNECTED);
	if ((int)atomic_read(&(cb->state)) == ERROR) {
		printk(KERN_ERR "wait for CONNECTED state %d\n",
										atomic_read(&(cb->state)));
		return -1;
	}

	MSGPRINTK("rdma_connect successful\n");
	// Jack: accep() done
	return 0;
}

/* Jack utility -
 *		  free the name buffer manually!
 */
uint32_t get_host_ip(char **tmp_net_dev_name) {
	struct net_device *device;
	struct in_device *in_dev;
	struct in_ifaddr *if_info;

	__u8 *addr;

	device = __dev_get_by_name(&init_net, net_dev_name); // namespace=normale
	if(device) {
		*tmp_net_dev_name = (char *) vmalloc(sizeof(net_dev_name));
		memcpy(*tmp_net_dev_name, net_dev_name, sizeof(net_dev_name));
		in_dev = (struct in_device *)device->ip_ptr;
		if_info = in_dev->ifa_list;
		addr = (char *)&if_info->ifa_local;
		
		MSGDPRINTK(KERN_WARNING "Device %s IP: %u.%u.%u.%u\n",
											net_dev_name, 
											(__u32)addr[0], (__u32)addr[1],
											(__u32)addr[2], (__u32)addr[3]);
		return (addr[0]<<24 | addr[1]<<16 | addr[2]<<8 | addr[3]);
	}
	else{
		device = __dev_get_by_name(&init_net, net_dev_name2);
		if(device) {
			*tmp_net_dev_name = (char *) vmalloc(sizeof(net_dev_name2));
			memcpy(*tmp_net_dev_name, net_dev_name2, sizeof(net_dev_name2));
			in_dev = (struct in_device *)device->ip_ptr;
			if_info = in_dev->ifa_list;
			addr = (char *)&if_info->ifa_local;
			MSGDPRINTK(KERN_WARNING "Device2 %s IP: %u.%u.%u.%u\n",
											net_dev_name2,
											(__u32)addr[0], (__u32)addr[1],
											(__u32)addr[2], (__u32)addr[3]);
			return (addr[0]<<24 | addr[1]<<16 | addr[2]<<8 | addr[3]);
		}
		else {
			MSGPRINTK(KERN_ERR "ERROR - cannot find host ip (eth0/p7p1)\n");
			return -1;
		}
	}
}

static void fill_sockaddr(struct sockaddr_storage *sin, struct krping_cb *_cb)
{
	memset(sin, 0, sizeof(*sin));

	if(!_cb->server) { // TODO: whould it be a potential bug?
		if (_cb->addr_type == AF_INET) { // client: load as usuall (ip=remote)
			struct sockaddr_in *sin4 = (struct sockaddr_in *)sin;
			sin4->sin_family = AF_INET;
			memcpy((void *)&sin4->sin_addr.s_addr, _cb->addr, 4);
			//cb[i]->port = htons(PORT+my_nid);
			sin4->sin_port = _cb->port;
		}
		KRPRINT_INIT("client IP fillup _cb->addr %s _cb->port %d\n",
														_cb->addr, _cb->port);
	}
	else { // cb->server: load from global (ip=itself)
		if (cb[my_nid]->addr_type == AF_INET) {
			struct sockaddr_in *sin4 = (struct sockaddr_in *)sin;
			sin4->sin_family = AF_INET;
			memcpy((void *)&sin4->sin_addr.s_addr, cb[my_nid]->addr, 4);
			sin4->sin_port = cb[my_nid]->port;
			KRPRINT_INIT("server IP fillup cb[my_nid]->addr %s cb[my_nid]->port %d\n",
											cb[my_nid]->addr, cb[my_nid]->port);
		}
	}
	/*
	else if (cb->addr_type == AF_INET6) {
		struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)sin;
		sin6->sin6_family = AF_INET6;
		memcpy((void *)&sin6->sin6_addr, cb->addr, 16);
		sin6->sin6_port = cb->port;
	} */
}

/*
 *	  IB/mlx5: Remove support for IB_DEVICE_LOCAL_DMA_LKEY (FASTREG)
 */
static int reg_supported(struct ib_device *dev)
{
	u64 needed_flags = IB_DEVICE_MEM_MGT_EXTENSIONS |
			   IB_DEVICE_LOCAL_DMA_LKEY;
	struct ib_device_attr device_attr;
	int ret;
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
		printk(KERN_ERR
			"Fastreg not supported - device_cap_flags 0x%llx\n",
			(u64)device_attr.device_cap_flags);
		return 1; // let it pass
	}
	MSGDPRINTK("Fastreg/local_dma_lkey supported - device_cap_flags 0x%llx\n",
											(u64)device_attr.device_cap_flags);
	return 1;
}

static int krping_bind_server(struct krping_cb *cb)
{
	struct sockaddr_storage sin;
	int ret;

	fill_sockaddr(&sin, cb);

	MSGPRINTK("rdma_bind_addr\n");
	ret = rdma_bind_addr(cb->cm_id, (struct sockaddr *)&sin);
	if (ret) {
		printk(KERN_ERR "rdma_bind_addr error %d\n", ret);
		return ret;
	}

	MSGPRINTK("rdma_listen\n");
	ret = rdma_listen(cb->cm_id, 99); // TODO: don't hardcode
	if (ret) {
		printk(KERN_ERR "rdma_listen failed: %d\n", ret);
		return ret;
	}

	return 0;
}

static void krping_setup_wr(struct krping_cb *cb) // set up sgl, used for rdma
{
	int i=0, ret;
	MSGPRINTK("\n\n\n->%s(): \n", __func__);
	MSGPRINTK("@@@ 2 cb->recv_size = %d\n", cb->recv_size);
	for(i=0;i<MAX_RECV_WR;i++) {
		struct ib_recv_wr *bad_wr;
		struct ib_recv_wr *_rq_wr = create_recv_wr(cb->conn_no, false);

		if(i<5 || i>(MAX_RECV_WR-5)) {
			MSGPRINTK("_rq_wr %p cb->conn_no %d recv_size %d wr_id %p\n",
			(void*)_rq_wr, cb->conn_no, cb->recv_size, (void*)_rq_wr->wr_id);
		}

		ret = ib_post_recv(cb->qp, _rq_wr, &bad_wr);
		if (ret) {
			printk(KERN_ERR "ib_post_recv failed: %d\n", ret);
			BUG();
		}
	}

	cb->send_sgl.addr = cb->send_dma_addr; // addr
	cb->send_sgl.length = sizeof cb->send_buf;
	//cb->send_sgl.lkey = cb->qp->device->local_dma_lkey; // A BUG from kprint.c

	//3.
	cb->send_sgl.lkey	   = cb->pd->local_dma_lkey; // correct
	//4.
	//cb->send_sgl.lkey = cb->reg_mr->lkey;	 // A BUG from kprint.c

	MSGPRINTK("@@@ <send addr>\n");
	MSGDPRINTK("@@@ cb->send_sgl.addr = %p\n", (void*)cb->send_sgl.addr);
		// this is not local_recv_buffer // it's exhanged local addr to remote
	MSGDPRINTK("@@@ cb->send_dma_addr = %p\n", (void*)cb->send_dma_addr);	
									// user vaddr (O) mapped to the next line
	MSGDPRINTK("@@@ cb->send_buf.payload = %p\n", cb->send_buf.payload);	 
												// kernel space buf (X) our msg
	MSGDPRINTK("@@@ sizeof(cb->send_buf) = %ld\n", sizeof cb->send_buf);	 
									// kernel addr (X) (mapped to each other)
	MSGDPRINTK("@@@ cb->recv_size = %d\n", cb->recv_size);	// kernel addr (X)

	MSGDPRINTK("@@@ <lkey>\n");
	MSGDPRINTK("@@@ cb->qp->device->local_dma_lkey = %d\n",
				cb->qp->device->local_dma_lkey);		//0
	MSGDPRINTK("@@@ lkey=%d from ../mad.c (ctx->pd->local_dma_lkey)\n",
							cb->pd->local_dma_lkey);	//4450 (dynamic diff)
	MSGDPRINTK("@@@ lkey=%d from client/server example(cb->mr->lkey)\n",
							cb->reg_mr->lkey);		  //4463 (dynamic diff)

	cb->sq_wr.opcode = IB_WR_SEND;				// normal send / recv
	cb->sq_wr.send_flags = IB_SEND_SIGNALED;	// check
	cb->sq_wr.sg_list = &cb->send_sgl;			// sge
	cb->sq_wr.num_sge = 1;

	MSGDPRINTK("anoter rdma buffer (passive buffer)\n");
	
	/* active: active_dma_addr; passive: passive_dma_addr */
	// READ/WRITE passive buf //
	cb->rdma_sgl.addr = cb->passive_dma_addr;

	cb->rdma_sq_wr.wr.sg_list = &cb->rdma_sgl;
	cb->rdma_sq_wr.wr.send_flags = IB_SEND_SIGNALED;
	cb->rdma_sq_wr.wr.num_sge = 1;

	/*
	 * A chain of 2 WRs, INVALDATE_MR + REG_MR.
	 * both unsignaled.  The client uses them to reregister
	 * the rdma buffers with a new key each iteration.
	 */
	cb->reg_mr_wr.wr.opcode = IB_WR_REG_MR;			//(legacy:fastreg)
	cb->reg_mr_wr.mr = cb->reg_mr;

	cb->reg_mr_wr_passive.wr.opcode = IB_WR_REG_MR;	//(legacy:fastreg)
	cb->reg_mr_wr_passive.mr = cb->reg_mr_passive;

	cb->invalidate_wr.next = &cb->reg_mr_wr.wr;		//
	cb->invalidate_wr.opcode = IB_WR_LOCAL_INV;		// invalidate Memory Window

	cb->invalidate_wr_passive.next = &cb->reg_mr_wr_passive.wr;
	cb->invalidate_wr_passive.opcode = IB_WR_LOCAL_INV;
	/*  The reg mem_mode uses a reg mr on the client side for the
	 *  rw_passive_buf and rw_active_buf buffers.  Each time the client will 
	 *  advertise one of these buffers, it invalidates the previous registration 
	 *  and fast registers the new buffer with a new key.
	 *
	 *  If the server_invalidate
	 *  option is on, then the server will do the invalidation via the
	 * "go ahead" messages using the IB_WR_SEND_WITH_INV opcode. Otherwise the
	 * client invalidates the mr using the IB_WR_LOCAL_INV work request.
	 */
	return;
}

static int krping_setup_qp(struct krping_cb *cb, struct rdma_cm_id *cm_id)
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
	cb->cq =
		ib_create_cq(cm_id->device, krping_cq_event_handler, NULL, cb, &attr);
	if (IS_ERR(cb->cq)) {
		printk(KERN_ERR "ib_create_cq failed\n");
		ret = PTR_ERR(cb->cq);
		goto err1;
	}
	MSGPRINTK("created cq %p task\n", cb->cq);

	ret = ib_req_notify_cq(cb->cq, IB_CQ_NEXT_COMP); // INT flag/mask raised
	if (ret) {
		printk(KERN_ERR "ib_create_cq failed\n");
		goto err2;
	}

	ret = krping_create_qp(cb);
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
static int krping_setup_buffers(struct krping_cb *cb)
{
	int ret;
	MSGPRINTK("\n->%s();\n", __func__);
	MSGPRINTK("krping_setup_buffers called on cb %p\n", cb);

	/* recv wq has been changed to be dinamically allocated */
	
	// send //
	cb->send_dma_addr = dma_map_single(cb->pd->device->dma_device,
															// for remote access
							   &cb->send_buf, sizeof(cb->send_buf), 
							   								// for local access
							   DMA_BIDIRECTIONAL);			// cb->send_dma
	pci_unmap_addr_set(cb, send_mapping, cb->send_dma_addr);
								// cb->send_mapping = cb->send_dma_addr
	/* active rw */
	//cb->rw_active_buf = kmalloc(cb->rdma_size, GFP_DMA);	// (X) GFP_DMA
	cb->rw_active_buf = kmalloc(cb->rdma_size, GFP_KERNEL);	// vmalloc(X)
								// (O) alloc rdma buffer (used for W/R)
	if (!cb->rw_active_buf) {
		MSGPRINTK("rw_active_buf malloc failed\n");
		ret = -ENOMEM;
		goto bail;
	}
	cb->active_dma_addr = dma_map_single(cb->pd->device->dma_device,
						   cb->rw_active_buf, cb->rdma_size,
						   DMA_BIDIRECTIONAL);
	pci_unmap_addr_set(cb, rdma_mapping, cb->active_dma_addr);

	cb->page_list_len = (((cb->rdma_size - 1) & PAGE_MASK) + PAGE_SIZE)
															>> PAGE_SHIFT;
	/* mr for active */
	cb->reg_mr = ib_alloc_mr(cb->pd, IB_MR_TYPE_MEM_REG,// fill up lkey and rkey
										 cb->page_list_len);
	/* mr for passive */
	cb->reg_mr_passive = ib_alloc_mr(cb->pd, IB_MR_TYPE_MEM_REG,
										 cb->page_list_len);

	if (IS_ERR(cb->reg_mr)) {
		ret = PTR_ERR(cb->reg_mr);
		MSGPRINTK("reg_mr failed %d\n", ret);
		goto bail;
	}
	if (IS_ERR(cb->reg_mr_passive)) {
		ret = PTR_ERR(cb->reg_mr_passive);
		MSGPRINTK("reg_mr_passive failed %d\n", ret);
		goto bail;
	}

	MSGPRINTK("@@@ 1 cb->rw_active_buf = dma_map_single( cb->rw_active_buf ) = "
						"0x%p kmalloc (mapping together)\n", cb->rw_active_buf);
	MSGPRINTK("@@@ 1 cb->active_dma_addr = 0x%p a kernel vaddr for remote "
					"access  (mapping together)\n", (void*)cb->active_dma_addr);
	MSGDPRINTK("\n@@@ after mr\n");
	MSGDPRINTK("@@@ reg rkey %d page_list_len %u\n",
										cb->reg_mr->rkey, cb->page_list_len);
	MSGDPRINTK("@@@ 1 Jack cb->reg_mr->lkey %d from mr \n", cb->reg_mr->lkey);
	MSGDPRINTK("@@@ 1 correct lkey=%d (ref: ./drivers/infiniband/core/mad.c )"
				"(ctx->pd->local_dma_lkey)\n", cb->pd->local_dma_lkey);
																//4xxx dynamic
	/* info
	//MSGPRINTK("@@@ 1 cb->send_sgl.lkey %d from mr \n", cb->send_sgl.lkey); //0
	//MSGPRINTK("@@@ 1 cb->recv_sgl.lkey %d from mr \n", cb->recv_sgl.lkey); //0
		// these are all NULL.
		cb->dma_mr->lkey, cb->dma_mr->rkey	  // null
		cb->rdma_mr->lkey, cb->rdma_mr->rkey	// null
		cb->start_mr->lkey, cb->start_mr->rkey  // null
	*/

	/* passive rw */
	MSGPRINTK("only client(not true) setup rw_passive_buf passive_dma_addr\n");
	cb->rw_passive_buf = kmalloc(cb->rdma_size, GFP_KERNEL); // vmalloc(X)
	if (!cb->rw_passive_buf) {
		MSGPRINTK("rw_passive_buf malloc failed\n");
		ret = -ENOMEM;
		goto bail;
	}
	cb->passive_dma_addr = dma_map_single(cb->pd->device->dma_device,
					   cb->rw_passive_buf, cb->rdma_size,
					   DMA_BIDIRECTIONAL);
	pci_unmap_addr_set(cb, start_mapping, cb->passive_dma_addr);
	MSGPRINTK("@@@ cb->passive_dma_addr = 0x%p Jack (only client->not true)\n",
												(void*)cb->passive_dma_addr);

	krping_setup_wr(cb);
	MSGPRINTK("allocated & registered buffers done!\n");
	MSGPRINTK("\n\n");
	return 0;
bail:
	if (cb->reg_mr && !IS_ERR(cb->reg_mr))
		ib_dereg_mr(cb->reg_mr);
	if (cb->reg_mr_passive && !IS_ERR(cb->reg_mr_passive))
		ib_dereg_mr(cb->reg_mr_passive);
	if (cb->rdma_mr && !IS_ERR(cb->rdma_mr))
		ib_dereg_mr(cb->rdma_mr);
	if (cb->dma_mr && !IS_ERR(cb->dma_mr))
		ib_dereg_mr(cb->dma_mr);
	if (cb->rw_active_buf)
		kfree(cb->rw_active_buf);
	if (cb->rw_passive_buf)
		kfree(cb->rw_passive_buf);
	return ret;
}


static int krping_accept(struct krping_cb *cb)
{
	struct rdma_conn_param conn_param;
	int ret;
	MSGPRINTK("\n->%s(); cb->conn_%d accepting client connection request....\n",
														__func__, cb->conn_no);
	memset(&conn_param, 0, sizeof conn_param);
	conn_param.responder_resources = 1;
	conn_param.initiator_depth = 1;

	ret = rdma_accept(cb->child_cm_id, &conn_param);
	if (ret) {
		printk(KERN_ERR "rdma_accept error: %d\n", ret);
		return ret;
	}

		MSGPRINTK("%s(): wating for a signal...............\n", __func__);
		wait_event_interruptible(cb->sem,
					atomic_read(&(cb->state)) == CONNECTED);
													// have a look child_cm_id
		MSGPRINTK("%s(): got the signal !!!!(GOOD)!!!!!!! cb->state = %d \n",
										__func__, atomic_read(&(cb->state)));
		if (atomic_read(&(cb->state)) == ERROR) {
			printk(KERN_ERR "wait for CONNECTED state %d\n",
												atomic_read(&(cb->state)));
			return -1;
		}

	//is_connection_done[cb->conn_no] = 1;
	set_popcorn_node_online(cb->conn_no);
	smp_mb(); // since my_nid is externed (global)
	MSGPRINTK("acception done!\n");
	return 0;
}

static void krping_free_buffers(struct krping_cb *cb)
{
	MSGPRINTK("krping_free_buffers called on cb %p\n", cb);

	if (cb->dma_mr)
		ib_dereg_mr(cb->dma_mr);
	if (cb->rdma_mr)
		ib_dereg_mr(cb->rdma_mr);
	if (cb->start_mr)
		ib_dereg_mr(cb->start_mr);
	if (cb->reg_mr)
		ib_dereg_mr(cb->reg_mr);
	if (cb->reg_mr_passive)
		ib_dereg_mr(cb->reg_mr_passive);

	/*
	dma_unmap_single(cb->pd->device->dma_device,
			 pci_unmap_addr(cb, recv_mapping),
			 //sizeof(&cb->recv_buf[0]), DMA_BIDIRECTIONAL);
			 //sizeof(cb->recv_buf)*MAX_RECV_WR, DMA_BIDIRECTIONAL);
			 sizeof(cb->recv_buf[0])*MAX_RECV_WR, DMA_BIDIRECTIONAL);
	*/
	dma_unmap_single(cb->pd->device->dma_device,
			 pci_unmap_addr(cb, send_mapping),
			 sizeof(cb->send_buf), DMA_BIDIRECTIONAL);
	dma_unmap_single(cb->pd->device->dma_device,
			 pci_unmap_addr(cb, rdma_mapping),
			 cb->rdma_size, DMA_BIDIRECTIONAL);
	kfree(cb->rw_active_buf);
	if (cb->rw_passive_buf) {
		dma_unmap_single(cb->pd->device->dma_device,
			 pci_unmap_addr(cb, start_mapping),
			 cb->rdma_size, DMA_BIDIRECTIONAL);
		kfree(cb->rw_passive_buf);
	}
}

static void krping_free_qp(struct krping_cb *cb)
{
	ib_destroy_qp(cb->qp);
	ib_destroy_cq(cb->cq);
	ib_dealloc_pd(cb->pd);
}

int krping_persistent_server_thread(void* arg0)
{
	struct krping_cb *cb = arg0;
	//struct ib_recv_wr *bad_wr;
	int ret = -1;

	MSGPRINTK("--thread--> %s(): conn %d\n", __func__, cb->conn_no);
	ret = krping_setup_qp(cb, cb->child_cm_id);
	if (ret) {
		printk(KERN_ERR "setup_qp failed: %d\n", ret);
		goto err0;
	}

	ret = krping_setup_buffers(cb);
	if (ret) {
		printk(KERN_ERR "krping_setup_buffers failed: %d\n", ret);
		goto err1;
	}
	// so far you can do send/recv

	ret = krping_accept(cb);
	if (ret) {
		printk(KERN_ERR "connect error %d\n", ret);
		goto err2;
	}

	//is_connection_done[cb->conn_no] = 1; // atomic is more safe
	set_popcorn_node_online(cb->conn_no);
	printk("conn_no %d is ready (GOOD)\n", cb->conn_no);

	return 0;
		//TODO: copy to outside the function
		rdma_disconnect(cb->child_cm_id); // used for rmmod
	err2:
		krping_free_buffers(cb);
	err1:
		krping_free_qp(cb);
	err0:
		rdma_destroy_id(cb->child_cm_id);
		return ret;
}

static int krping_run_server(void* arg0)
{
	struct krping_cb *_cb, *listening_cb = arg0;
	struct task_struct *t;
	int ret, i = 1;

	MSGPRINTK("<<< %s(): cb->conno %d >>>\n", __func__, listening_cb->conn_no);
	MSGPRINTK("<<< %s(): cb->conno %d >>>\n", __func__, listening_cb->conn_no);
	MSGPRINTK("<<< %s(): cb->conno %d >>>\n", __func__, listening_cb->conn_no);

	ret = krping_bind_server(listening_cb);
	if (ret)
		return ret;

	MSGPRINTK("\n\n\n");

	//TODO: MCJack-modify outside
	//- create multiple connections -//
	while(1){
		/* Wait for client's Start STAG/TO/Len */
		msleep(1000);
		wait_event_interruptible(listening_cb->sem,
					atomic_read(&(listening_cb->state)) == CONNECT_REQUEST);
		if (atomic_read(&(listening_cb->state)) != CONNECT_REQUEST) {
			printk(KERN_ERR "wait for CONNECT_REQUEST state %d\n",
										atomic_read(&(listening_cb->state)));
			continue;
		}
		KRPRINT_INIT("Got a connection\n");

		/* create a thread for this */
		//exec_thread_data *exec_data = (exec_thread_data *) 
		//						kmalloc(sizeof(exec_thread_data), GFP_KERNEL);
		//exec_data->conn_no = conn_no;
		//TODO: catch return thread
		//struct krping_cb* cb = (struct krping_cb*)
							//* (cb + (sizeof(struct krping_cb)*(my_nid))
							//+ (sizeof(struct krping_cb)*(i)));
		_cb = cb[my_nid+i];
		_cb->server=1;

		KRPRINT_INIT("1 _cb->conn_no %d\n", _cb->conn_no);
		KRPRINT_INIT("2 cb[my_nid] %p cb[my_nid]->child_cm_id %p\n",
										cb[my_nid], cb[my_nid]->child_cm_id);
		KRPRINT_INIT("2 cb[my_nid+i] %p cb[my_nid+i]->child_cm_id %p\n",
									cb[my_nid+i], cb[my_nid+i]->child_cm_id);

		KRPRINT_INIT("3 _cb->child_cm_id %p = cb_listen->child_cm_id %p "
								"(Jack!!!!!!1/31)\n",
								_cb->child_cm_id, cb_listen->child_cm_id);

		_cb->child_cm_id = cb_listen->child_cm_id; 
							// will be used [setup_qp(SRWRirq)] -> setup_buf ->

		KRPRINT_INIT("3 _cb->child_cm_id %p = cb_listen->child_cm_id %p "
								"(Jack!!!!!!1/31)\n",
								_cb->child_cm_id, cb_listen->child_cm_id);
		t = kthread_run(krping_persistent_server_thread, _cb,
									"krping_persistent_server_conn_thread");
		BUG_ON(IS_ERR(t));

		atomic_set(&listening_cb->state, IDLE);
		i++;
	}
	return 0;
}

static int krping_bind_client(struct krping_cb *cb)
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
								atomic_read(&(cb->state)) == ROUTE_RESOLVED);
	if (atomic_read(&(cb->state)) != ROUTE_RESOLVED) {
		printk(KERN_ERR "addr/route resolution did not resolve: state %d\n",
													atomic_read(&(cb->state)));
		return -EINTR;
	}

	if (!reg_supported(cb->cm_id->device)) //Jack
		return -EINVAL;

	MSGPRINTK("rdma_resolve_addr - rdma_resolve_route successful\n");
	return 0;
}

static int krping_create_qp(struct krping_cb *cb)
{
	struct ib_qp_init_attr init_attr;
	int ret;

	memset(&init_attr, 0, sizeof(init_attr));
	init_attr.cap.max_send_wr = cb->txdepth;	//
	init_attr.cap.max_recv_wr = MAX_RECV_WR*2;	// TODO: +++++++++++++++++++

	/* For flush_qp() */
	init_attr.cap.max_send_wr++;
	init_attr.cap.max_recv_wr++;

	init_attr.cap.max_recv_sge = 1;	 // ok for now
	init_attr.cap.max_send_sge = 1;	 // ok for now
	init_attr.qp_type = IB_QPT_RC;
	init_attr.send_cq = cb->cq;		 // send and recv use a same cq
	init_attr.recv_cq = cb->cq;
	init_attr.sq_sig_type = IB_SIGNAL_REQ_WR;

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


///////////////////////////rdma nead/////////////////////////////////////////
// can happen simultaneously
static void handle_remote_thread_rdma_read_request(
									struct pcn_kmsg_long_message* inc_lmsg)
{
	remote_thread_rdma_rw_request_t* request = 
								(remote_thread_rdma_rw_request_t*) inc_lmsg;
	remote_thread_rdma_rw_request_t *reply; 
	int ret;
	struct ib_send_wr *bad_wr, inv; // for ib_post_send
	struct krping_cb *_cb = cb[request->header.from_nid];
	volatile unsigned long ts_start, ts_compose, ts_post, ts_end;
#ifdef CONFIG_POPCORN_DEBUG_MSG_LAYER_VERBOSE
	int dbg;
#endif

	MSGDPRINTK("%s():\n", __func__);
#ifdef CONFIG_POPCORN_DEBUG_MSG_LAYER_VERBOSE
	MSGPRINTK("<<<<< Jack passive READ request: "
				"my_nid=%d from_nid=%d rw_t %d recv_ticket %lu "
				"r_rdma_ticket %d msg_layer(good) >>>>>\n",
								my_nid, request->header.from_nid, 
											request->rw_ticket,
											request->header.ticket,
											request->rdma_ticket);
#endif
				
	/* ib client  sending read key to [remote server] */
	// get key, and connjuct to the cb
	MSGDPRINTK("RPC passive READ request\n");

	/* send         ---->   irq (recv)
	 *                      [lock R]
	 *                      perform WRITE
	 *                      unlock R
	 * irq (recv)  <-----   send
	 */    
	 
	//MSG_SYNC_PRK("//// READ passive   lock() wait %d (active) rw_t %d ////\n",
	//                                    (int)atomic_read(&_cb->passive_cnt),
	//                                    request->rw_ticket);// rdms dbg
	mutex_lock(&_cb->passive_mutex); // passive side
	atomic_inc(&_cb->passive_cnt);

#ifdef CONFIG_POPCORN_DEBUG_MSG_LAYER_VERBOSE
	MSG_SYNC_PRK("////// READ passive   lock() %d (active) rw_t %d ////////\n",
										(int)atomic_read(&_cb->passive_cnt),
										request->rw_ticket);// rdms dbg
#endif

	// perform READ (passive side)
	// performance evaluation
	// <time1 : compose msg info>
	rdtscll(ts_start);

	// remote info:
	_cb->remote_rkey = ntohl(request->remote_rkey);		// redaundant
	_cb->remote_addr = ntohll(request->remote_addr);	// redaundant
	_cb->remote_len = request->rw_size;				// redaundant

	_cb->rdma_sq_wr.rkey = _cb->remote_rkey;		// updated from remote!!!!
	_cb->rdma_sq_wr.remote_addr = _cb->remote_addr;	// updated from remote!!!!
	_cb->rdma_sq_wr.wr.sg_list->length = _cb->remote_len;
	MSGPRINTK("<<<<< READ request: my_nid %d from_nid %d "
					"remote_rkey %d remote_addr %p rw_size %d>>>>>\n", 
											my_nid, request->header.from_nid,
											_cb->remote_rkey,
											(void*)_cb->remote_addr,
											_cb->remote_len);
	
	// local info:
	//_active_dma_addr -> passive_dma_addr
	// register local buf for performing R/W (rdma_rkey)
	_cb->rdma_sgl.lkey = krping_rdma_rkey_passive(_cb, _cb->passive_dma_addr, 
											!_cb->read_inv, _cb->remote_len);
	//  TODO: rdma_sgl will conflict each other (R&W) but rdma_send protects.
	_cb->rdma_sq_wr.wr.next = NULL; // a work request

	if (unlikely(_cb->read_inv))
		_cb->rdma_sq_wr.wr.opcode = IB_WR_RDMA_READ_WITH_INV;
	else { 
		/* Compose a READ sge with a invalidation */
		_cb->rdma_sq_wr.wr.opcode = IB_WR_RDMA_READ;
		/* Immediately follow the read with a
		 * fenced LOCAL_INV. */
		_cb->rdma_sq_wr.wr.next = &inv; // followed by a inv
		memset(&inv, 0, sizeof inv);
		inv.opcode = IB_WR_LOCAL_INV;
		//inv.ex.invalidate_rkey = _cb->reg_mr->rkey;
		inv.ex.invalidate_rkey = _cb->reg_mr_passive->rkey;
		inv.send_flags = IB_SEND_FENCE;
	}

	MSG_RDMA_PRK("ib_post_send R>>>>\n");
	// <time 2: send>
	rdtscll(ts_compose);

	mutex_lock(&_cb->qp_mutex);
	ret = ib_post_send(_cb->qp, &_cb->rdma_sq_wr.wr, &bad_wr);
	mutex_unlock(&_cb->qp_mutex);
	if (ret) {
		printk(KERN_ERR "post send error %d\n", ret);
		return;;
	}
	_cb->rdma_sq_wr.wr.next = NULL;

	// <time 3: send request done>
	rdtscll(ts_post);

	/* Wait for read completion */
	wait_event_interruptible(_cb->sem, 
				(int)atomic_read(&(_cb->read_state)) == RDMA_READ_COMPLETE);
	/* passive READ done */
	atomic_set(&_cb->read_state, IDLE);

	// <time 4: READ(task) done>
	rdtscll(ts_end);

	/* time result */
	DEBUG_LOG("R: %d K compose_time %lu post_time %lu "
										"end_time %lu (cpu ticks)\n",
										(request->rw_size+1)/1024, 
										ts_compose-ts_start, // +1 end char
										ts_post-ts_start, ts_end-ts_start);

	/**************/
	/* READ DEBUG */
	/**************/
	// READ: check data (check here not response())
	DEBUG_LOG("<<<<< rpc (passive) R_READ DONE %ld "
						"g_test_remote_len (?) (really good) \n",
						strlen(_cb->rw_passive_buf)); // passive
	DEBUG_LOG("<<<<< rpc request_size %d done_size %ld "
						"_cb->rw_passive_buf(first10) \"%.10s\"\n", 
						request->rw_size, 
						strlen(_cb->rw_passive_buf), _cb->rw_passive_buf);

	//TODO:try uncomment this
	//memset(cb->rw_passive_buf, '\n', strlen(_cb->rw_passive_buf));
#ifdef CONFIG_POPCORN_DEBUG_MSG_LAYER_VERBOSE
	dbg = request->rdma_ticket;
#endif
	/* send ---->   irq
	 *              lock R
	 *              perform READ
	 *              [unlock R]
	 * irq  <-----  send
	 * 
	 */
#ifdef CONFIG_POPCORN_DEBUG_MSG_LAYER_VERBOSE
	MSG_SYNC_PRK("/////// READ passive unlock() %d (active) rw_t %d ///////\n",
										(int)atomic_read(&_cb->passive_cnt),
										request->rw_ticket);// rdms dbg
#endif
										
	mutex_unlock(&_cb->passive_mutex); // passive side

	MSG_RDMA_PRK("%s(): send READ COMPLETION ACK !!! -->>\n", __func__); 
	reply = pcn_kmsg_alloc_msg(sizeof(*reply));
	if(!reply)
		BUG_ON(-1);
#ifdef CONFIG_POPCORN_DEBUG_MSG_LAYER_VERBOSE
	reply->rdma_ticket = dbg; // dbg
	reply->rw_ticket = request->rw_ticket;
#endif
	reply->header.type = PCN_KMSG_TYPE_RDMA_READ_RESPONSE;
	reply->header.prio = PCN_KMSG_PRIO_NORMAL;
	//reply->tgroup_home_cpu = tgroup_home_cpu;
	//reply->tgroup_home_id = tgroup_home_id;

	// meaning it's a rdma msg == is_rdma
	reply->is_rdma = true;
	((remote_thread_rdma_rw_request_t*) reply)
											->remote_rkey  = _cb->remote_rkey;
	((remote_thread_rdma_rw_request_t*) reply)
											->remote_addr  = _cb->remote_addr;
	((remote_thread_rdma_rw_request_t*) reply)
											->rw_size    = _cb->remote_len;

	// RDMA R/W complete ACK
	reply->rdma_ack = true;     // activator: 1 passive: 0

	ib_kmsg_send_long(request->header.from_nid, 
						(struct pcn_kmsg_long_message*) reply, 
						sizeof(*reply));

	MSGPRINTK("%s(): end\n", __func__);
	pcn_kmsg_free_msg(reply);
	pcn_kmsg_free_msg(inc_lmsg);
	return;
}

/////////////////////////rdma write/////////////////////////////////////////

static void handle_remote_thread_rdma_write_request(
								struct pcn_kmsg_long_message* inc_lmsg)
{
	remote_thread_rdma_rw_request_t* request = 
								(remote_thread_rdma_rw_request_t*) inc_lmsg;
	remote_thread_rdma_rw_request_t *reply; 
	unsigned long ts_wr_start, ts_wr_compose, ts_wr_post, ts_wr_end;
	struct krping_cb *_cb = cb[request->header.from_nid];
	struct ib_send_wr *bad_wr; // for ib_post_send
	int ret;

#ifdef CONFIG_POPCORN_DEBUG_MSG_LAYER_VERBOSE
	MSGPRINTK("<<<<< Jack passive WRITE request: my %d from %d rw_t %d "
				"ticket %lu rdma_ticket %d  >>>>>\n", 
				my_nid, request->header.from_nid, request->rw_ticket,
				request->header.ticket, request->rdma_ticket);
#endif
	/* ib client  sending write key to [remote server] */
	// get key, and connjuct to the cb
	MSGDPRINTK("<<<<< rpc (remote request) r_write(remotely write)\n"); 

	/* send         ---->   irq (recv)
	 *                      [lock]
	 *                      perform READ
	 * irq (recv)  <-----   send
	 *                      unlock
	 */

	mutex_lock(&_cb->passive_mutex);
	atomic_inc(&_cb->passive_cnt);
	MSG_SYNC_PRK("/////////// WRITE passive lock() %d /////////////////\n",
										(int)atomic_read(&_cb->passive_cnt));


	// perform WRITE (passive side) + performance evaluation
	// <time1 : compose msg info>
	rdtscll(ts_wr_start);

	/* RDMA Write echo data */
	 _cb->remote_rkey = ntohl(request->remote_rkey);     // redaundant
	 _cb->remote_addr = ntohll(request->remote_addr);    // redaundant
	 _cb->remote_len = request->rw_size;               // redaundant

	_cb->rdma_sq_wr.rkey = _cb->remote_rkey;        // updated from remote!!!!
	_cb->rdma_sq_wr.remote_addr = _cb->remote_addr; // updated from remote!!!!
	_cb->rdma_sq_wr.wr.sg_list->length = _cb->remote_len;

	_cb->rdma_sq_wr.wr.opcode = IB_WR_RDMA_WRITE;

	// register local buf for performing R/W (rdma_rkey)
	_cb->rdma_sgl.lkey = krping_rdma_rkey_passive(
						_cb, _cb->passive_dma_addr, 1, _cb->remote_len);
	//  TODO: rdma_sgl will conflict each other (R&W)
	// Jack: cb->rdma_sq_wr.wr.sg_list = &cb->rdma_sgl;



	MSGPRINTK("<<<<< WRITE request: my_nid %d from_nid %d, "
						"lkey %d laddr %llx _cb->rdma_sgl.lkey %d, "
						"remote_rkey %d remote_addr %p rw_size %d>>>>>\n", 
						my_nid, request->header.from_nid, 
						_cb->rdma_sq_wr.wr.sg_list->lkey,
						(unsigned long long)_cb->rdma_sq_wr.wr.sg_list->addr,
						_cb->rdma_sgl.lkey,
						_cb->remote_rkey,
						(void*)_cb->remote_addr,
						_cb->remote_len);
						//request->header.remote_rkey, 
						//(void*)request->header.remote_addr, 
						//request->header.rw_size);
	
	MSG_RDMA_PRK("ib_post_send W>>>>\n");
	// <time 2: send>
	rdtscll(ts_wr_compose);

	mutex_lock(&_cb->qp_mutex);
	ret = ib_post_send(_cb->qp, &_cb->rdma_sq_wr.wr, &bad_wr);
	mutex_unlock(&_cb->qp_mutex);
	if (ret) {
		printk(KERN_ERR "post send error %d\n", ret);
		return;
	}
	// <time 3: send request done>
	rdtscll(ts_wr_post);

	/* Wait for completion */
	ret = wait_event_interruptible(_cb->sem, 
				(int)atomic_read(&(_cb->write_state)) == RDMA_WRITE_COMPLETE);
	atomic_set(&_cb->write_state, IDLE);
	// <time 4: WRITE(task) done>
	rdtscll(ts_wr_end);
	/*
	if ((int)atomic_read(&(_cb->write_state)) != RDMA_WRITE_COMPLETE) {
		printk(KERN_ERR "wait for RDMA_WRITE_COMPLETE Wstate %d\n", 
								(int)atomic_read(&(_cb->write_state)));
		BUG_ON((int)atomic_read(&(_cb->write_state)));
	}
	*/
	/* passive WRITE done */

	/* time result */ 
	DEBUG_LOG("W: %d K compose_time %lu post_time %lu "
			"end_time %lu (cpu ticks)\n", 
			(((remote_thread_rdma_rw_request_t*) request)->rw_size+1)/1024, 
			ts_wr_compose-ts_wr_start, // +1 end char
			ts_wr_post-ts_wr_start, ts_wr_end-ts_wr_start);

	/* WRITE DEBUG */
	// nooe (check in response)

	/* send ---->   irq
					lock W
	 *              perform WRITE
					[unlock W]
	 * irq  <-----  send
	 */
	MSG_SYNC_PRK("///////////// WRITE passive unlock() %d /////////////////\n",
										(int)atomic_read(&_cb->passive_cnt));
	mutex_unlock(&_cb->passive_mutex); // passive side

	/* send W completion ACK */
	DEBUG_LOG("send WRITE COMPLETION ACK\n");
	reply = pcn_kmsg_alloc_msg(sizeof(*reply));
	if(!reply)
		BUG_ON(-1);

	reply->header.type = PCN_KMSG_TYPE_RDMA_WRITE_RESPONSE;
	reply->header.prio = PCN_KMSG_PRIO_NORMAL;
	//reply->tgroup_home_cpu = tgroup_home_cpu;
	//reply->tgroup_home_id = tgroup_home_id;

	// RDMA W/R complete ACK
	((remote_thread_rdma_rw_request_t*) reply)->remote_rkey  
														= _cb->remote_rkey;
	((remote_thread_rdma_rw_request_t*) reply)->remote_addr  
														= _cb->remote_addr;
	((remote_thread_rdma_rw_request_t*) reply)->rw_size   
														= _cb->remote_len;

	// RDMA W/R complete ACK
	reply->rdma_ack = true;     // activator: 1 passive: 0

	ib_kmsg_send_long(request->header.from_nid, 
				(struct pcn_kmsg_long_message*) reply, sizeof(*reply));
	
	MSGPRINTK("%s(): end\n\n\n", __func__);
	pcn_kmsg_free_msg(reply);
	pcn_kmsg_free_msg(inc_lmsg);
	return; 
}


/* action for bottom half
 * handler no longer has to kfree the lmsg !!
 */
static void pcn_kmsg_handler_BottomHalf(struct work_struct * work)
{
	pcn_kmsg_work_t *w = (pcn_kmsg_work_t *) work;
	struct pcn_kmsg_long_message *lmsg;
	pcn_kmsg_cbftn ftn;
	
	MSGPRINTK("%s(): \n", __func__);

	// main
	lmsg = vmalloc(w->lmsg.header.size);
	if(!lmsg) {
		printk(KERN_ERR "CANNOT ALLOC\n");
		BUG();
	}
	memcpy(lmsg, &w->lmsg, w->lmsg.header.size);


	if( lmsg->header.type < 0 || lmsg->header.type >= PCN_KMSG_TYPE_MAX) {
		printk(KERN_ERR "Received invalid message type %d > MAX %d\n",
									lmsg->header.type, PCN_KMSG_TYPE_MAX);
	}
	else {
#ifdef CONFIG_POPCORN_DEBUG_MSG_LAYER_VERBOSE
#if 0
		// you don't know whether is a rdma request or not
		printk(" Grabing to callbacks kwq->lmsg->header.type %d %s "
							"kwq->lmsg->header.size %d is_rdma %d rw_t %d\n",
							lmsg->header.type,
							lmsg->header.type==2?"REQUEST":"RESPONSE",
							lmsg->header.size,
							(int)lmsg->is_rdma,
							lmsg->rw_ticket);
#endif
#endif
		/* normal msg */
		ftn = callbacks[lmsg->header.type];
		if(ftn != NULL) {
			ftn((void*)lmsg);
			//ftn((void*)&w->lmsg);
		} else {
			MSGPRINTK(KERN_INFO "Recieved message type %d size %d "
									"has no registered callback!\n",
									lmsg->header.type, lmsg->header.size);
			BUG_ON(-1);
		}
	}
	MSGPRINTK("%s(): done & free everything\n\n", __func__);
	
	kfree((void*)w);
	return;
}

/*
 * parse recved msg in the buf to msg_layer
 * in INT
 */
static int ib_kmsg_recv_long(struct krping_cb *cb,
							 struct wc_struct *wcs)
{
	struct pcn_kmsg_long_message *lmsg = wcs->element_addr;
	pcn_kmsg_work_t *kmsg_work;

	if(unlikely( lmsg->header.size > sizeof(struct pcn_kmsg_long_message))) {
		printk(KERN_ERR "Received invalide message size > MAX %lu\n", 
									sizeof(struct pcn_kmsg_long_message));
		BUG();
	}

	DEBUG_LOG("%s(): producing BottomHalf wc->wr_id = lmsg %p header.size %d\n",
						__func__, (void*)lmsg, lmsg->header.size);

	// - alloc & cpy msg to kernel buffer
	kmsg_work = kmalloc(sizeof(pcn_kmsg_work_t), GFP_ATOMIC);
	if (likely(kmsg_work)) {

#ifdef CONFIG_POPCORN_DEBUG_MSG_LAYER_VERBOSE
		MSG_RDMA_PRK("bf: Spwning BottomHalf, leaving INT "
						"kwq->lmsg->header.type %d %s "
						"kwq->lmsg->header.size %d rw_t %d\n",
						lmsg->header.type,
						lmsg->header.type==2?"REQUEST":"RESPONSE",
						lmsg->header.size,
						((remote_thread_rdma_rw_request_t*)lmsg)->rw_ticket);
#endif

	if(unlikely(!memcpy(&kmsg_work->lmsg, lmsg, lmsg->header.size))) {
			BUG_ON(-1);
	}

#ifdef CONFIG_POPCORN_DEBUG_MSG_LAYER_VERBOSE
		kmsg_work->lmsg.header.ticket = atomic_inc_return(&g_recv_ticket);
		MSGPRINTK("%s() recv ticket %lu\n", 
								__func__, kmsg_work->lmsg.header.ticket);
#endif

#ifdef CONFIG_POPCORN_DEBUG_MSG_LAYER_VERBOSE
		MSG_RDMA_PRK("af: Spwning BottomHalf, leaving INT "
				"kwq->lmsg->header.type %d %s "
				"kwq->lmsg->header.size %d\n",
				kmsg_work->lmsg.header.type,
				kmsg_work->lmsg.header.type==2?"REQUEST":"RESPONSE",
				kmsg_work->lmsg.header.size);
#endif 

		INIT_WORK((struct work_struct *)kmsg_work, pcn_kmsg_handler_BottomHalf);
		if(unlikely(!queue_work(msg_handler, (struct work_struct *)kmsg_work)))
		   BUG();
	} else {
		printk("Failed to kmalloc work structure!\n");
		BUG_ON(-1);
	}

	kfree(lmsg);
	kfree(wcs->recv_sgl);
	kfree(wcs->rq_wr);
	kfree(wcs);
	return 0;
}

static int krping_run_client(struct krping_cb *cb)
{
	int ret;

	MSGPRINTK("<<<<<<<< %s(): cb->conno %d >>>>>>>>\n", __func__, cb->conn_no);
	MSGPRINTK("<<<<<<<< %s(): cb->conno %d >>>>>>>>\n", __func__, cb->conn_no);
	MSGPRINTK("<<<<<<<< %s(): cb->conno %d >>>>>>>>\n", __func__, cb->conn_no);
	ret = krping_bind_client(cb);
	if (ret)
		return ret;

	ret = krping_setup_qp(cb, cb->cm_id);
	if (ret) {
		printk(KERN_ERR "setup_qp failed: %d\n", ret);
		return ret;
	}

	ret = krping_setup_buffers(cb);
	if (ret) {
		printk(KERN_ERR "krping_setup_buffers failed: %d\n", ret);
		goto err1;
	}

	/*
	MSGPRINTK("ib_post_recv(manually)<<<<\n");
	//ret = ib_post_recv(cb->qp, &cb->rq_wr, &bad_wr);
	ret = ib_post_recv(cb->qp, &cb->rq_wr[0], &bad_wr); ///////////////////
	if (ret) {
		printk(KERN_ERR "ib_post_recv failed: %d\n", ret);
		goto err2;
	}
	*/

	ret = krping_connect_client(cb);
	if (ret) {
		printk(KERN_ERR "connect error %d\n", ret);
		goto err2;
	}
	return 0;

	//TODO: copy to outside the function
	rdma_disconnect(cb->cm_id); // used for rmmod
err2:
	krping_free_buffers(cb);
err1:
	krping_free_qp(cb);
	return ret;
}

static void handle_remote_thread_rdma_read_response(
										struct pcn_kmsg_long_message* inc_lmsg)
{
	remote_thread_rdma_rw_request_t* response =
								(remote_thread_rdma_rw_request_t*) inc_lmsg;
	struct krping_cb *_cb = cb[response->header.from_nid];

	// example
	//response->header.rw_size;	  // if send/recv, this = 0
	//response->header.rdma_ack;	   // activator: 1 passive: 0

#ifdef CONFIG_POPCORN_DEBUG_MSG_LAYER_VERBOSE
	// DBG for READ - show the data remote side should see
	// printk out the msg info remote side should know
	//TODO: NO NEED  rw_active_buf -> rw_passive_buf, 
	// because this is just a ack showing what 
	// should be seen in the passive(remote) side
	// active side provide original message
	DEBUG_LOG("%s(): response->header.rw_size %d "
								"_cb->rw_active_buf(first10)%.10s "
								"rdma_ack %s(==true)\n",
								__func__, response->rw_size,
								_cb->rw_active_buf + response->rw_size-10,
								response->rdma_ack?"true":"false");

	DEBUG_LOG("response->header.remote_rkey %u remote_addr %p rw_size %u "
							"rw_t %d recv_ticket %lu ack_rdma_ticket %d\n",
									response->remote_rkey,
									(void*)response->remote_addr,
									response->rw_size,
									response->rw_ticket,		// rdma dbg
									response->header.ticket,	// send/recv dbg
									response->rdma_ticket); 	// rdma dbg
#endif

#ifdef CONFIG_POPCORN_DEBUG_MSG_LAYER_VERBOSE
	MSG_SYNC_PRK("///////READ active unlock() %d rw_t %d conn %d///////////\n",
										(int)atomic_read(&_cb->active_cnt),
										response->rw_ticket,
										_cb->conn_no);
#endif                                        
	mutex_unlock(&_cb->active_mutex);

	MSGPRINTK("%s(): end\n", __func__);
	pcn_kmsg_free_msg(inc_lmsg);
	return;
}

static void handle_remote_thread_rdma_write_response(
										struct pcn_kmsg_long_message* inc_lmsg)
{
	remote_thread_rdma_rw_request_t* response =
								(remote_thread_rdma_rw_request_t*) inc_lmsg;
	struct krping_cb *_cb = cb[response->header.from_nid];

#ifdef CONFIG_POPCORN_DEBUG_MSG_LAYER_VERBOSE
	// Check WRITE result (dbg)
	DEBUG_LOG("%s(): response->header.rw_size %d "
								"_cb->rw_passive_buf(first10)%.10s "
								"rdma_ack %s(==true)\n",
								__func__, response->rw_size,
								_cb->rw_passive_buf+response->rw_size-10,
								response->rdma_ack?"true":"false");
    DEBUG_LOG("response->header.remote_rkey %u remote_addr %p rw_size %u "
									"rw_t %d ticket %lu rdma_ticket %d\n",
												response->remote_rkey,
												(void*)response->remote_addr,
												response->rw_size,
												response->rw_ticket,
												response->header.ticket,
												response->rdma_ticket);
#endif

	/*
	// check - an example (for write)
	//if(memcmp(g_test_buf, _cb->rw_active_buf, g_test_remote_len)) {
	if(memcmp(g_test_buf, _cb->rw_active_buf, response->rw_size)) {
		printk("%s(): RDMA read data the same", __func__, );
	}
	else {
		printk("%s(): RDMA read data the same", __func__, );
	}
	//clean test buf ()
	//memset(_cb->rw_active_buf, 0,1);
	*/

	// copy rw_active_buf to user assign address
	// transffer your_buf_put to here
	// memcpy(_your_buf_put, cb->rw_active_buf, respnse->rw_size);
	// bottomhalf to user_registered_fun()

	MSG_SYNC_PRK("/////////////WRITE active unlock() %d////////////////\n",
											(int)atomic_read(&_cb->active_cnt));
	mutex_unlock(&_cb->active_mutex);

	MSGPRINTK("%s(): end\n\n\n", __func__);
	pcn_kmsg_free_msg(inc_lmsg);
	return;
}


// Initialize callback table to null, set up control and data channels
int __init initialize()
{
	int i, err, conn_no;
	char *tmp_net_dev_name=NULL;
	struct task_struct *t;
	// TODO: check how to assign a priority to these threads! 
	// make msg_layer faster (higher prio)
	// struct sched_param param = {.sched_priority = 10};
	KRPRINT_INIT("--- Popcorn messaging layer init starts ---\n");

	KRPRINT_INIT("Registering softirq handler (BottomHalf)...\n");
	//open_softirq(PCN_KMSG_SOFTIRQ, pcn_kmsg_action); 
	// TODO: check open_softirq()
	msg_handler = create_workqueue("MSGHandBotm"); // per-cpu

	for(i=0; i<MAX_NUM_NODES; i++) {
		// find my name
		if ( get_host_ip(&tmp_net_dev_name) == ip_table2[i])  {
			my_nid=i;
			KRPRINT_INIT("Device \"%s\" my_nid=%d on machine IP %u.%u.%u.%u\n",
												tmp_net_dev_name, my_nid,
												(ip_table2[i]>>24)&0x000000ff,
												(ip_table2[i]>>16)&0x000000ff,
												(ip_table2[i]>> 8)&0x000000ff,
												(ip_table2[i]>> 0)&0x000000ff);
			vfree(tmp_net_dev_name);
			break;
		}
	}
	if(my_nid==-99)
		BUG_ON("my_nid isn't initialized\n");

	smp_mb(); // since my_nid is extern (global)
	KRPRINT_INIT("---------------------------------------------------------\n");
	KRPRINT_INIT("---- updating to my_nid=%d wait for a moment ----\n", my_nid);
	KRPRINT_INIT("---------------------------------------------------------\n");
	KRPRINT_INIT("MSG_LAYER: Initialization my_nid=%d\n", my_nid);

	for (i = 0; i<MAX_NUM_NODES; i++) {
		set_popcorn_node_offline(i);
	}

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
		MSGDPRINTK("cb[%d] %p\n", i, cb[i]);

		conn_no=i;
		// 0. create and save to list
		
		cb[i] = kzalloc(sizeof(struct krping_cb), GFP_KERNEL); // TODO: release

		mutex_lock(&krping_mutex);
		list_add_tail(&cb[i]->list, &krping_cbs); // multiple cb!!!
		mutex_unlock(&krping_mutex);

		// settup node number
		cb[i]->conn_no = i;

		// setup ip

		// setup locks
		mutex_init(&cb[i]->send_mutex);
		mutex_init(&cb[i]->recv_mutex);
		mutex_init(&cb[i]->active_mutex);
		mutex_init(&cb[i]->passive_mutex);
		mutex_init(&cb[i]->qp_mutex);

		// 1. init comment parameters //
		cb[i]->state.counter = 1;	   // IDLE
		cb[i]->send_state.counter = 1;
		cb[i]->recv_state.counter = 1;
		cb[i]->read_state.counter = 1;
		cb[i]->write_state.counter = 1;

		cb[i]->active_cnt.counter = 0;
		cb[i]->passive_cnt.counter = 0;

		g_rw_ticket.counter = 0;
		g_send_ticket.counter = 0;
		g_recv_ticket.counter = 0;

		/* for sync prob */
		cb[i]->g_all_ticket.counter = 0;
		spin_lock_init(&cb[i]->rw_slock);

		cb[i]->stats.send_msgs.counter = 0;
		cb[i]->stats.recv_msgs.counter = 0;
		cb[i]->stats.write_msgs.counter = 0;
		cb[i]->stats.read_msgs.counter = 0;

		cb[i]->rdma_size = MAX_RDMA_SIZE;

		init_waitqueue_head(&cb[i]->sem); // init waitq for wait, wake up
		cb[i]->txdepth = RPING_SQ_DEPTH;

		// 2. init uncomment parameters //
		// server/client/myself
		cb[i]->server = -1; // -1: myself

		// set up IPv4 address
		cb[i]->addr_str = ip_table[conn_no];
		in4_pton(ip_table[conn_no], -1, cb[i]->addr, -1, NULL); // will be used
		cb[i]->addr_type = AF_INET;				// [IPv4]/V6 // for determining
		cb[i]->port = htons(PORT);		// sock always the same port, not for ib
		KRPRINT_INIT("ip_table[conn_no] %s, cb[i]->addr_str %s, "
										 "cb[i]->addr %s,  port %d\n",
										 ip_table[conn_no], cb[i]->addr_str,
													 cb[i]->addr, (int)PORT);

		// register event handler
		cb[i]->cm_id = rdma_create_id(&init_net, krping_cma_event_handler,
											cb[i], RDMA_PS_TCP, IB_QPT_RC);
		if (IS_ERR(cb[i]->cm_id)) {
			err = PTR_ERR(cb[i]->cm_id);
			printk(KERN_ERR "rdma_create_id error %d\n", err);
			goto out;
		}
		KRPRINT_INIT("created cm_id %p (pair to event handler)\n",
															cb[i]->cm_id);

		cb[i]->recv_size = sizeof(struct pcn_kmsg_long_message);

		cb[i]->server_invalidate = 0;
		cb[i]->read_inv = 0;

		//TODO: remove these all
		//memcpy(cb[i]->addr, ip_table[i], sizeof(ip_table[i]));
		//mutex_init(&cb[i]->read_mutex);
		//mutex_init(&cb[i]->write_mutex);
		//mutex_init(&cb[i]->sync_prob);
		//mutex_init(&sync_prob);
		//spin_lock_init(&splock);
		//cb[i]->irq.counter = 0;
		//g_ticket.counter = 0;

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

	//- One persistent listening server -//
	cb_listen = cb[my_nid];
	cb_listen->server = 1;
	// 1. [my_nid(accept)], connect
	t = kthread_run(krping_run_server, cb_listen,
					"krping_persistent_server_listen_thread");
	BUG_ON(IS_ERR(t));

	//is_connection_done[my_nid] = 1;
	set_popcorn_node_online(my_nid);

	//- client -//
	for (i=0; i<MAX_NUM_NODES; i++) {
		if (i==my_nid) {
			continue; // has done (server)
		}

		conn_no = i;	 // Take node 1 for example. connect0 [1] accept2
		if (conn_no < my_nid) { // 1. my_nid(accept), [connect]
			cb[conn_no]->server = 0;

			// server/client dependant init
			msleep(1000);  // TODO: replace this with wait/completion
			err = krping_run_client(cb[conn_no]); // connect_to()
			if (err) {
				printk("WRONG!!\n");
				return err;
			}

			//is_connection_done[conn_no] = 1; //Jack: atomic is more safe
			set_popcorn_node_online(conn_no);
			smp_mb(); // Jack: calling it one time in the end should be fine
		}
		else{
			MSGPRINTK("no action needed for conn %d "
					   "(listening will take care)\n", i);
		}
		// TODO: Jack: use kthread run?
		//sched_setscheduler(<kthread_run()'s return>, SCHED_FIFO, &param);
		//set_cpus_allowed_ptr(<kthread_run()'s return>, cpumask_of(i%NR_CPUS));
	}

	for ( i=0; i<MAX_NUM_NODES; i++ ) {
		while ( !is_popcorn_node_online(i) ) {
			MSGDPRINTK("waiting for is_popcorn_node_online(%d)\n", i);
			//MSGDPRINTK("waiting for is_connection_done[%d]\n", i);
			msleep(3000);
			//io_schedule();
		}
	}

	// load
	if(!SMART_IB_MSG) {
		send_callback = (send_cbftn)ib_kmsg_send_long;  // send()
		send_callback_rdma = (send_cbftn)ib_kmsg_send_rdma; // rdma read/write()
	} else {
		send_callback = (send_cbftn)ib_kmsg_send_smart;
	}
	MSGPRINTK("Value of send ptr = %p\n", send_callback);
	MSGPRINTK("--- Popcorn messaging layer is up ---\n");


	// TODO: move to an earlier place
	MSGPRINTK("--- init all ib[]->state ---\n");
	for ( i=0; i<MAX_NUM_NODES; i++ ) {
		atomic_set(&cb[i]->state, IDLE);
		atomic_set(&cb[i]->send_state, IDLE);
		atomic_set(&cb[i]->recv_state, IDLE);
		atomic_set(&cb[i]->read_state, IDLE);
		atomic_set(&cb[i]->write_state, IDLE);
	}

	/* Make init popcorn call */
	//_init_RemoteCPUMask(); // msg boradcast //Jack: deal w/ it later

	/* testing is in another module msg_layer_test.ko */

	/* register RDMA must-have callbacks */
	pcn_kmsg_register_callback(
					(enum pcn_kmsg_type)PCN_KMSG_TYPE_RDMA_READ_REQUEST,
					(pcn_kmsg_cbftn)handle_remote_thread_rdma_read_request);
	pcn_kmsg_register_callback(
					(enum pcn_kmsg_type)PCN_KMSG_TYPE_RDMA_READ_RESPONSE,
					(pcn_kmsg_cbftn)handle_remote_thread_rdma_read_response);

	pcn_kmsg_register_callback(
					(enum pcn_kmsg_type)PCN_KMSG_TYPE_RDMA_WRITE_REQUEST,
					(pcn_kmsg_cbftn)handle_remote_thread_rdma_write_request);
	pcn_kmsg_register_callback(
					(enum pcn_kmsg_type)PCN_KMSG_TYPE_RDMA_WRITE_RESPONSE,
					(pcn_kmsg_cbftn)handle_remote_thread_rdma_write_response);

	smp_mb();
	MSGPRINTK(KERN_INFO "Popcorn Messaging Layer Initialized\n"
														"\n\n\n\n\n\n\n\n");
	return 0;

out:
	for(i=0; i<MAX_NUM_NODES; i++){
		//if(cb[i]->state!=0) { // making sure it hasbeen inited
		if(atomic_read(&(cb[i]->state))) {
			mutex_lock(&krping_mutex);
			list_del(&cb[i]->list);
			mutex_unlock(&krping_mutex);
			kfree(cb[i]);
			// TODO: cut connections
		}
	}
	return err;
}

/*
 * return the (possibly rebound) rkey for the rdma buffer.
 * REG mode: invalidate and rebind via reg wr.
 * other modes: just return the mr rkey.
 */
u32 krping_rdma_rkey(struct krping_cb *_cb, u64 buf, int post_inv, int rdma_len)
{
	u32 rkey;
	struct ib_send_wr *bad_wr;
	int ret;
	struct scatterlist sg = {0};

	// old key
	_cb->invalidate_wr.ex.invalidate_rkey = _cb->reg_mr->rkey;
													// save corrent reg rkey

	// Update the reg key.
	ib_update_fast_reg_key(_cb->reg_mr, _cb->key); // keeps the key the same
	_cb->reg_mr_wr.key = _cb->reg_mr->rkey;

	//
	// Setup permissions
	//
	// reg_mr_wr_passive in another function
	_cb->reg_mr_wr.access = IB_ACCESS_REMOTE_READ   |
							IB_ACCESS_REMOTE_WRITE  |
							IB_ACCESS_LOCAL_WRITE   |
							IB_ACCESS_REMOTE_ATOMIC; // unsafe but works

	sg_dma_address(&sg) = buf;		// passed by caller
	sg_dma_len(&sg) = rdma_len;		// R/W length
	DEBUG_LOG("%s(): rdma_len (dynamical) %d\n", __func__, sg_dma_len(&sg));

	//ret = ib_map_mr_sg(cb->reg_mr, &sg, 1, NULL, PAGE_SIZE);
	ret = ib_map_mr_sg(_cb->reg_mr, &sg, 1, PAGE_SIZE);
										// snyc ib_dma_sync_single_for_cpu/dev
	BUG_ON(ret <= 0 || ret > _cb->page_list_len);

	DEBUG_LOG("%s(): ### post_inv = %d, reg_mr new rkey %d pgsz %u len %u"
			" rdma_len (dynamical) %d iova_start %llx\n", __func__, post_inv,
			_cb->reg_mr_wr.key, _cb->reg_mr->page_size, _cb->reg_mr->length,
			rdma_len, _cb->reg_mr->iova);

	mutex_lock(&_cb->qp_mutex);
	if (likely(post_inv)) // becaus remote doesn't have inv, so manual? then W?
		ret = ib_post_send(_cb->qp, &_cb->invalidate_wr, &bad_wr);
	else
		; //ret = ib_post_send(_cb->qp, &_cb->reg_mr_wr.wr, &bad_wr);
												// by passive WRITE in krping.c
	mutex_unlock(&_cb->qp_mutex);

	if (ret) {
		printk(KERN_ERR "post send error %d\n", ret);
		//cb->state = ERROR;
		atomic_set(&_cb->state, ERROR);
		//TODO ALL / corresponding to the request
		atomic_set(&_cb->send_state, ERROR);
		atomic_set(&_cb->recv_state, ERROR);
		atomic_set(&_cb->read_state, ERROR);
		atomic_set(&_cb->write_state, ERROR);
	}

	rkey = _cb->reg_mr->rkey; // TODO: check this [cb->reg_mr->rkey]
	return rkey;
}
EXPORT_SYMBOL(krping_rdma_rkey);

u32 krping_rdma_rkey_passive(struct krping_cb *_cb,
							 u64 buf, int post_inv, int rdma_len)
{
	u32 rkey;
	struct ib_send_wr *bad_wr;
	int ret;
	struct scatterlist sg = {0};

	// store old key
	_cb->invalidate_wr_passive.ex.invalidate_rkey = _cb->reg_mr_passive->rkey;
														// save corrent reg rkey

	// Update the reg key.
	ib_update_fast_reg_key(_cb->reg_mr_passive, _cb->key);
													// keeps the key the same
	_cb->reg_mr_wr_passive.key = _cb->reg_mr_passive->rkey;

	_cb->reg_mr_wr_passive.access = IB_ACCESS_REMOTE_READ   |
									IB_ACCESS_REMOTE_WRITE  |
									IB_ACCESS_LOCAL_WRITE   |
									IB_ACCESS_REMOTE_ATOMIC; // unsafe but works

	sg_dma_address(&sg) = buf;		// passed by caller
	sg_dma_len(&sg) = rdma_len;		// R/W length

	ret = ib_map_mr_sg(_cb->reg_mr_passive, &sg, 1, PAGE_SIZE);  
										// snyc ib_dma_sync_single_for_cpu/dev
	BUG_ON(ret <= 0 || ret > _cb->page_list_len);

	MSG_RDMA_PRK("%s(): ### post_inv = %d, reg_mr_wr_passive new rkey %d "
				 "pgsz %u len %u rdma_len (dynamical) %d iova_start %llx\n",
				 __func__, post_inv, _cb->reg_mr_wr_passive.key,
				 _cb->reg_mr_passive->page_size, _cb->reg_mr_passive->length,
										rdma_len, _cb->reg_mr_passive->iova);

	mutex_lock(&_cb->qp_mutex);
	if (post_inv)
		ret = ib_post_send(_cb->qp, &_cb->invalidate_wr_passive, &bad_wr);
	else
		ret = ib_post_send(_cb->qp, &_cb->reg_mr_wr_passive.wr, &bad_wr); 
												// called by only passive WRITE
	mutex_unlock(&_cb->qp_mutex);

	if (ret) {
		printk(KERN_ERR "post send error %d\n", ret);
		//cb->state = ERROR;
		atomic_set(&_cb->state, ERROR);
		//TODO ALL / corresponding to the request
		atomic_set(&_cb->send_state, ERROR);
		atomic_set(&_cb->recv_state, ERROR);
		atomic_set(&_cb->read_state, ERROR);
		atomic_set(&_cb->write_state, ERROR);
	}

	rkey = _cb->reg_mr_passive->rkey; // TODO: check this [cb->reg_mr->rkey]
	return rkey;
}
EXPORT_SYMBOL(krping_rdma_rkey_passive);

/*
 * Your request must be done by kmalloc().
 * You have to free by yourself
 */
int ib_kmsg_send_rdma(unsigned int dest_cpu, struct pcn_kmsg_long_message *lmsg,
					  unsigned int rw_size)
{
	uint32_t rkey;
	MSGDPRINTK("%s(): \n", __func__);

	// info setup
	((remote_thread_rdma_rw_request_t*) lmsg)->is_rdma = true;
	((remote_thread_rdma_rw_request_t*) lmsg)->rw_size = rw_size;

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
#ifdef CONFIG_POPCORN_DEBUG_MSG_LAYER_VERBOSE
	if(lmsg->header.type == PCN_KMSG_TYPE_RDMA_READ_REQUEST) {
		MSG_SYNC_PRK("///////READ active   lock() %d rw_t %d conn %d ///////\n",
									(int)atomic_read(&cb[dest_cpu]->active_cnt),
							((remote_thread_rdma_rw_request_t*)lmsg)->rw_ticket,
														cb[dest_cpu]->conn_no);
	}
	else if(lmsg->header.type == PCN_KMSG_TYPE_RDMA_WRITE_REQUEST) {
		MSG_SYNC_PRK("////////WRITE active lock() %d  rw_t %d conn %d //////\n",
									(int)atomic_read(&cb[dest_cpu]->active_cnt),
							((remote_thread_rdma_rw_request_t*)lmsg)->rw_ticket,
														cb[dest_cpu]->conn_no);
		}
	else
		BUG();
#endif
	
	mutex_lock(&cb[dest_cpu]->active_mutex);

#ifdef CONFIG_POPCORN_DEBUG_MSG_LAYER_VERBOSE
	((remote_thread_rdma_rw_request_t*)lmsg)->rw_ticket = 
								atomic_inc_return(&cb[dest_cpu]->g_all_ticket);
#endif
	
#ifdef CONFIG_POPCORN_DEBUG_MSG_LAYER_VERBOSE
	atomic_inc(&cb[dest_cpu]->active_cnt);  // lock dbg
#endif

	/* form rdma meta data */
	// active_dma_addr(rw_active_buf) for active, start for passive
	MSGPRINTK("krping_format_W/R info(): \n"); // composing a W/R ACK (active)
	rkey = krping_rdma_rkey(cb[dest_cpu], cb[dest_cpu]->active_dma_addr,
										!cb[dest_cpu]->server_invalidate, 
					((remote_thread_rdma_rw_request_t*) lmsg)->rw_size);
							// active_dma_addr is sent for remote READ/WRITE
													// failed to trun inv off
	
	((remote_thread_rdma_rw_request_t*) lmsg)->remote_addr = 
										htonll(cb[dest_cpu]->active_dma_addr);
	((remote_thread_rdma_rw_request_t*) lmsg)->remote_rkey = htonl(rkey);
	MSGPRINTK("%s(): - @@@ cb[%d] rkey %d cb[]->active_dma_addr %p "
												"lmsg->rw_size %d\n",
												__func__, dest_cpu, rkey,
									(void*)cb[dest_cpu]->active_dma_addr, 
					((remote_thread_rdma_rw_request_t*) lmsg)->rw_size);

	lmsg->header.from_nid = my_nid;
	((remote_thread_rdma_rw_request_t*) lmsg)->rdma_ack = false;

	if(dest_cpu == my_nid) {
		printk(KERN_ERR "No support for sending msg to itself %d\n", dest_cpu);
		printk(KERN_ERR "No support for sending msg to itself %d\n", dest_cpu);
		printk(KERN_ERR "No support for sending msg to itself %d\n", dest_cpu);
		return 0;
	}

	// pcn_msg (abstraction msg layer)
	//----------------------------------------------------------
	// ib
#ifdef CONFIG_POPCORN_DEBUG_MSG_LAYER_VERBOSE
	((remote_thread_rdma_rw_request_t*)lmsg)->rdma_ticket = 
									atomic_inc_return(&g_rw_ticket); // from 1
	MSGPRINTK("%s(): rw ticket %d\n",
			__func__, ((remote_thread_rdma_rw_request_t*)lmsg)->rdma_ticket);
#endif

	// copy from kernel to rw_active_buf for remote to read
	if(!((remote_thread_rdma_rw_request_t*) lmsg)->is_write) {
		if(unlikely(!memcpy( cb[dest_cpu]->rw_active_buf, 
						((remote_thread_rdma_rw_request_t*) lmsg)->your_buf_ptr,
						((remote_thread_rdma_rw_request_t*) lmsg)->rw_size))) {
			printk(KERN_ERR "READ/WRITE(): BAD memcpy\n");
			BUG();
		}
	}
	
	// send signal/request
	ib_kmsg_send_long(dest_cpu, 
						(struct pcn_kmsg_long_message*) lmsg, sizeof(*lmsg));

	MSGPRINTK("Jackmsglayer: which is 1 rdma request\n");
	return 0;
}

/*
 * User doesn't have to take care of concurrency problem.
 * This func will take care of it.
 * User has to free the allocated mem manually since they can reuse their buf
 */
int ib_kmsg_send_long(unsigned int dest_cpu,
					  struct pcn_kmsg_long_message *lmsg,
					  unsigned int msg_size)
{
	int ret;
	struct ib_send_wr *bad_wr;

	lmsg->header.size = msg_size;

#ifdef CONFIG_POPCORN_DEBUG_MSG_LAYER_VERBOSE
#if 0
	MSGPRINTK("%s(): - msg_size %d sizeof(lmsg->header) %ld ack %s\n",
							__func__, msg_size, sizeof(lmsg->header),
									lmsg->rdma_ack?"true":"false");
#endif
#endif

	// check size with ib_send window size
	if( lmsg->header.size > sizeof(struct pcn_kmsg_long_message)) {
		printk("%s(): ERROR - MSG %d larger than MAX_MSG_SIZE %ld\n",
			__func__, lmsg->header.size, sizeof(struct pcn_kmsg_long_message));
		BUG();
	}

	lmsg->header.from_nid = my_nid;

	if(dest_cpu==my_nid) {
		printk(KERN_ERR "No support for sending msg to itself %d\n", dest_cpu);
		printk(KERN_ERR "No support for sending msg to itself %d\n", dest_cpu);
		printk(KERN_ERR "No support for sending msg to itself %d\n", dest_cpu);
		return 0;
	}


	// pcn_msg (abstraction msg layer)
	//----------------------------------------------------------
	// ib

	/* rdma w/r */
	if( ((remote_thread_rdma_rw_request_t*) lmsg)->remote_rkey &&
		((remote_thread_rdma_rw_request_t*) lmsg)->remote_addr &&
		((remote_thread_rdma_rw_request_t*) lmsg)->rw_size ) {
		; //it's a signal
	}
		
	MSG_SYNC_PRK("//////////////////lock() conn %d///////////////\n", dest_cpu);
	mutex_lock(&cb[dest_cpu]->send_mutex);

#ifdef CONFIG_POPCORN_DEBUG_MSG_LAYER_VERBOSE
	lmsg->header.ticket = atomic_inc_return(&g_send_ticket); // from 1
	MSGPRINTK("%s(): send ticket %lu\n", __func__, lmsg->header.ticket);
#endif

	// copy form kernel buf to ib buf (rw_passive_buf = send_buf!)
	if(unlikely(!memcpy(&cb[dest_cpu]->send_buf, lmsg, lmsg->header.size))) {
		printk(KERN_ERR "send(): BAD memcpy\n");
		BUG_ON(-1);
	}

	mutex_lock(&cb[dest_cpu]->qp_mutex);
	ret = ib_post_send(cb[dest_cpu]->qp, &cb[dest_cpu]->sq_wr, &bad_wr); 
					// sq_wr is hardcoded used for send&recv, rdma_sq_wr for W/R
	mutex_unlock(&cb[dest_cpu]->qp_mutex);

	//wait_event(cb[dest_cpu]->sem,
	wait_event_interruptible(cb[dest_cpu]->sem,
				atomic_read(&(cb[dest_cpu]->state)) == RDMA_SEND_COMPLETE);
	// since this // I don't have to have enq/deq()

	atomic_set(&cb[dest_cpu]->state, IDLE); // dont let it stay stay the state
	mutex_unlock(&cb[dest_cpu]->send_mutex);
	MSG_SYNC_PRK("//////////////unlock() conn %d///////////////\n", dest_cpu);
	MSGDPRINTK("Jackmsglayer: 1 msg sent to dest_cpu %d!!!!!!\n\n", dest_cpu);
	return 0;
}

int ib_kmsg_send_smart(unsigned int dest_cpu,
					  struct pcn_kmsg_long_message *lmsg,
					  unsigned int msg_size)
{
	if( msg_size > (sizeof(*lmsg)) ) {
		if( unlikely(msg_size > MAX_RDMA_SIZE) ) {
			printk(KERN_ERR "%s(): ERROR - R/W size %u "
							"is larger than MAX_RDMA_SIZE %d\n",
							__func__, msg_size, MAX_RDMA_SIZE);
			BUG();
		}
		return ib_kmsg_send_rdma(dest_cpu, lmsg, msg_size);
	}
	return ib_kmsg_send_long(dest_cpu, lmsg, msg_size);
}

/*
 *  Not yet done.
 */
static void __exit unload(void)
{
	int i;
	KRPRINT_INIT("Stopping kernel threads\n");

	KRPRINT_INIT("Release generals\n");
	for (i = 0; i<MAX_NUM_NODES; i++) {
	}

	/* release */
	KRPRINT_INIT("Release threadss\n");
	for (i = 0; i<MAX_NUM_NODES; i++) {
		//if(handler[i]!=NULL)
		//	kthread_stop(handler[i]);
		//if(sender_handler[i]!=NULL)
		//	kthread_stop(sender_handler[i]);
		//if(execution_handler[i]!=NULL)
		//	kthread_stop(execution_handler[i]);
		//Jack: TODO: sock release buffer, check(according to) the init
	}

	KRPRINT_INIT("Release IBs\n");
	for (i = 0; i<MAX_NUM_NODES; i++) {
	}

	KRPRINT_INIT("Successfully unloaded module!\n");
}

module_init(initialize);
module_exit(unload);
MODULE_LICENSE("GPL");
