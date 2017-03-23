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

#include <linux/pcn_kmsg.h>

/* rdma */
#include <rdma/ib_verbs.h>
#include "msg_layer2_rdma.c" // read&write process specific

#include "common.h"

/* Jack
 *  mssg layer multi-version
 *  msg sent to data_sock[conn_no] according to dest_cpu
 *  TODO:
 *		  test htonll and remove #define htonll(x) cpu_to_be64((x))
 *		  test ntohll and remove #define ntohll(x) cpu_to_be64((x))
 *		  my_cpu -> my_nid
 *		  is_connect();
 */

char net_dev_name[]="ib0";
const char net_dev_name2[]="p7p1"; //another special case, Xgene(ARM)

char* ip_table[] = { "192.168.69.127",	 // echo3 ib0
					 "192.168.69.128",	 // echo4 ib0
					 "192.168.69.129"};	// none ib0
// temporary solution................
uint32_t ip_table2[] = { (192<<24 | 168<<16 | 69<<8 | 127),	 // echo3 ib0
						 (192<<24 | 168<<16 | 69<<8 | 128),	 // echo4 ib0
						 (192<<24 | 168<<16 | 69<<8 | 129)};	// none ib0

/* dbg */ //TODO: add a dbg MARCO
#define MSG_TEST 	0
atomic_t g_rw_ticket;	// dbg, from1
atomic_t g_send_ticket;  // dbg, from1
atomic_t g_recv_ticket;  // dbg, from1

/*
 * HW info:
 * attr.cqe = cb->txdepth * 8;
 * - cq entries - indicating we want room for ten entries on the queue.
 *   This number should be set large enough that the queue isn’t overrun.
 */
#define RPING_SQ_DEPTH 128

#define PORT 1000
#define MAX_RECV_WR 500
#define MAX_RDMA_SIZE 4*1024*1024
LIST_HEAD(krping_cbs);
DEFINE_MUTEX(krping_mutex);

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

struct krping_stats {
	atomic_t send_msgs;
	atomic_t recv_msgs;
	atomic_t write_msgs;
	atomic_t read_msgs;
};

/* rq_wr -> wc
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
	int read_inv;
	u8 key;
	//struct ib_recv_wr rq_wr[MAX_RECV_WR];	 /* recv work request record */
	//struct ib_sge recv_sgl[MAX_RECV_WR];	  /* recv single SGE */
	//struct pcn_kmsg_long_message recv_buf[MAX_RECV_WR]; /* malloc'd buffer */ /* msg unit[] */
	int recv_size;

	u64 recv_dma_addr;
	//DECLARE_PCI_UNMAP_ADDR(recv_mapping) // cannot compile = =
	u64 recv_mapping;

	struct ib_send_wr sq_wr;	/* send work requrest record */
	struct ib_sge send_sgl;
	struct pcn_kmsg_long_message send_buf;  /* single send buf */ /* msg unit */
	u64 send_dma_addr;
	//DECLARE_PCI_UNMAP_ADDR(send_mapping) // cannot compile = =
	u64 send_mapping;

	struct ib_rdma_wr rdma_sq_wr;   /* rdma work request record */
	struct ib_sge rdma_sgl;		 /* rdma single SGE */

	/* a rdma buf for active */
	char *rdma_buf;		 /* used as rdma sink */
	u64  rdma_dma_addr;	 /* for active buffer */
	//DECLARE_PCI_UNMAP_ADDR(rdma_mapping) // cannot compile = =
	u64 rdma_mapping;
	struct ib_mr *rdma_mr;

	uint32_t remote_rkey;	   /* remote guys RKEY */
	uint64_t remote_addr;	   /* remote guys TO */
	uint32_t remote_len;		/* remote guys LEN */

	/* a rdma buf for passive */
	char *passive_buf;		  /* passive R/W buffer */
	u64  passive_dma_addr;	  /* passive R/W buffer addr*/
	//DECLARE_PCI_UNMAP_ADDR(start_mapping) // cannot compile = =
	u64 start_mapping;
	struct ib_mr *start_mr;
	//enum test_state state;	/* used for cond/signalling */
	atomic_t state;
	atomic_t send_state;
	atomic_t recv_state;
	atomic_t read_state;
	atomic_t write_state;
	//atomic_t irq;
	wait_queue_head_t sem;
	struct krping_stats stats;

	uint16_t port;		  /* dst port in NBO */
	u8 addr[16];			/* dst addr in NBO */
	char *addr_str;		 /* dst addr string */
	uint8_t addr_type;	  /* ADDR_FAMILY - IPv4/V6 */
	int verbose;			/* verbose logging */
	int count;			  /* ping count */
	//int size;			 /* ping data size */
	unsigned long rdma_size;	/* ping data size */
	int validate;			   /* validate ping data */
	int wlat;			   /* run wlat test */
	int rlat;			   /* run rlat test */
	int bw;				 /* run bw test */
	int duplex;			 /* run bw full duplex test */
	int poll;			   /* poll or block for rlat test */
	int txdepth;			/* SQ depth */
	int local_dma_lkey;	 /* use 0 for lkey */
	int frtest;			 /* reg test */

	/* CM stuff */
	struct rdma_cm_id *cm_id;	   /* connection on client side,*/
									/* listener on server side. */
	struct rdma_cm_id *child_cm_id; /* connection on server side */
	struct list_head list;
	int conn_no;

	/* sync */
	struct mutex send_mutex;
	struct mutex recv_mutex;
	struct mutex active_mutex;
	struct mutex passive_mutex; /* passive lock*/
	struct mutex qp_mutex;	  /* protect ib_post_send(qp)*/
	atomic_t active_cnt;		/* used for cond/signalling */
	atomic_t passive_cnt;	   /* used for cond/signalling */

	/* for sync problem */
	spinlock_t rw_slock;
	atomic_t g_all_ticket;
	atomic_t g_now_ticket;

	atomic_t g_wr_id;		   // for assigning wr_id
};



typedef struct {
	//struct pcn_kmsg_rdma_hdr hdr; /* must followd */
	struct pcn_kmsg_hdr hdr; /* must followd */
	/* you define */

}__attribute__((packed)) remote_thread_rdma_read_request_t; // for cache

typedef struct {
	//struct pcn_kmsg_rdma_hdr hdr; /* must followd */
	struct pcn_kmsg_hdr hdr; /* must followd */
	/* you define */

}__attribute__((packed)) remote_thread_rdma_write_request_t; // for cache

/* global */
extern struct krping_cb *cb[MAX_NUM_NODES];


/* utilities */
u32 krping_rdma_rkey(struct krping_cb *cb, u64 buf, int post_inv, int rdma_len);
u32 krping_rdma_rkey_passive(struct krping_cb *cb, u64 buf, int post_inv, int rdma_len);



/*
 * List of running krping threads.
 */
struct krping_cb *cb[MAX_NUM_NODES];
EXPORT_SYMBOL(cb);
struct krping_cb *cb_listen;

int ib_kmsg_send_long(unsigned int dest_cpu,
					  struct pcn_kmsg_long_message *lmsg,
					  unsigned int payload_size); // triggered by user, doing enq() and then sending signal
int ib_kmsg_send_rdma(unsigned int dest_cpu,
					  struct pcn_kmsg_rdma_message *lmsg,
					  unsigned int rdma_size);

/* TODO: obey popcorn-rack */
volatile static int is_connection_done[MAX_NUM_NODES]; //Jack: atomic is more safe / use smp_mb()

//* Popcorn utility *//
uint32_t get_host_ip(char **tmp_net_dev_name);
static int __init initialize(void);
extern pcn_kmsg_cbftn callbacks[PCN_KMSG_TYPE_MAX];
extern send_cbftn send_callback;
extern send_cbftn send_callback2;

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
	static int jack = 0;
	DEBUG_LOG("[[[[[external]]]]] conn_no %d (%s) >>>>>>>> %s(): "
			  "cma_event type %d cma_id %p (%s)\n", _cb->conn_no,
			(my_cpu == _cb->conn_no) ? "server" : "client", __func__,
			event->event, cma_id, (cma_id == _cb->cm_id) ? "parent" : "child");
	DEBUG_LOG("< cma_id %p _cb->cm_id %p >\n", cma_id, _cb->cm_id);

	switch (event->event) {
	case RDMA_CM_EVENT_ADDR_RESOLVED:
		DEBUG_LOG("< ------------RDMA_CM_EVENT_ADDR_RESOLVED------------ >\n");
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
		DEBUG_LOG("< -----CONNECT_REQUEST-----: _cb->child_cm_id %p = "
									"cma_id(external) >\n", _cb->child_cm_id);
		_cb->child_cm_id = cma_id; // distributed to other connections
		DEBUG_LOG("< -----CONNECT_REQUEST-----: _cb->child_cm_id %p = "
									"cma_id(external) >\n", _cb->child_cm_id);
		wake_up_interruptible(&_cb->sem);
		break;

	case RDMA_CM_EVENT_ESTABLISHED:
		DEBUG_LOG("< -------------CONNECTION ESTABLISHED---------------- >\n");
		atomic_set(&_cb->state, CONNECTED);

		if(cb[my_cpu]->conn_no == _cb->conn_no){
			jack++;
			DEBUG_LOG("< my business >\n");
			DEBUG_LOG("< cb[my_cpu]->conn_no %d _cb->conn_no %d "
						"jack %d >\n", cb[my_cpu]->conn_no, _cb->conn_no, jack);
		}
		else{
			DEBUG_LOG("< none of my business >\n");
			DEBUG_LOG("< cb[my_cpu]->conn_no %d _cb->conn_no %d "
						"jack %d >\n", cb[my_cpu]->conn_no, _cb->conn_no, jack);
		}
		is_connection_done[my_cpu+jack]=1;
		DEBUG_LOG("< %s(): _cb->state %d, CONNECTED %d >\n",
						__func__, (int)atomic_read(&(_cb->state)), CONNECTED);
		//wake_up(&_cb->sem); // TODO: testing: change back, see if it runs as well
		wake_up_interruptible(&_cb->sem); // default:
		break;

	case RDMA_CM_EVENT_ADDR_ERROR:
	case RDMA_CM_EVENT_ROUTE_ERROR:
	case RDMA_CM_EVENT_CONNECT_ERROR:
	case RDMA_CM_EVENT_UNREACHABLE:
	case RDMA_CM_EVENT_REJECTED:
		printk(KERN_ERR "< cma event %d, error %d >\n", event->event,
			   event->status);
		//_cb->state = ERROR;
		atomic_set(&_cb->state, ERROR);
		wake_up_interruptible(&_cb->sem);
		break;

	case RDMA_CM_EVENT_DISCONNECTED:
		printk(KERN_ERR "< -----DISCONNECT EVENT------... >\n");
		DEBUG_LOG("< %s(): _cb->state = %d, CONNECTED=%d >\n",
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

// Attention: could be in INT
struct ib_recv_wr* jack_func(int conn_no, bool is_int)
{
	struct krping_cb *_cb = cb[conn_no];
	struct pcn_kmsg_long_message *element_addr;
	struct ib_sge *_recv_sgl;
	struct ib_recv_wr *_rq_wr;
	struct wc_struct *wcs;

	/* allocation */
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
	u64 element_dma_addr = dma_map_single(_cb->pd->device->dma_device,
										  element_addr, _cb->recv_size,
													DMA_BIDIRECTIONAL);

	// set up sgl
	_recv_sgl->length = _cb->recv_size;
	_recv_sgl->addr = element_dma_addr;
	_recv_sgl->lkey = _cb->pd->local_dma_lkey;

	// set up rq_wr
	_rq_wr->sg_list = _recv_sgl;
	_rq_wr->num_sge = 1;
	//_rq_wr->wr_id = (u64)element_addr;
	_rq_wr->wr_id = (u64)wcs;
	_rq_wr->next = NULL;

	// save all address to release
	wcs->element_addr = element_addr;
	wcs->recv_sgl = _recv_sgl;
	wcs->rq_wr = _rq_wr;

	DEBUG_LOG_V("_rq_wr %p _cb->recv_size %d element_addr %p\n",
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

	/* // TODO: test
	if(unlikely(atomic_read(&_cb->irq)==1)) {
		printk(KERN_ERR "reentrance happens!!!!!!!!!!!!!!!!!!!\n");
		BUG();
	}
	atomic_set(&_cb->irq, 1);
	*/

	DEBUG_LOG("\n[[[[[external]]]]] node %d ------> %s\n",
									_cb->conn_no, __func__);

	BUG_ON(_cb->cq != cq);
	if (atomic_read(&(_cb->state)) == ERROR) {
		printk(KERN_ERR "< cq completion in ERROR state >\n");
		return;
	}

	while ((ret = ib_poll_cq(_cb->cq, 1, &wc)) > 0) {
		_wc = &wc;

		if (_wc->status) { // !=IBV_WC_SUCCESS
			if (_wc->status == IB_WC_WR_FLUSH_ERR) {
				DEBUG_LOG("< cq flushed >\n");
				//continue; // Jack: TODO: put it back for while(ib_poll_cq)
			} else {
				printk(KERN_ERR "< cq completion failed with "
					   "wr_id %Lx status %d opcode %d vender_err %x >"
					   "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n",
						_wc->wr_id, _wc->status, _wc->opcode, _wc->vendor_err);
				BUG_ON(_wc->status);
				goto error;
				//continue;
			}
		}

		switch (_wc->opcode) {
		case IB_WC_SEND:
			atomic_inc(&_cb->stats.send_msgs);
			EXP_LOG("<<< --- from %d [[[[[ SEND ]]]]] COMPLETION %d --- >>>\n",
								_cb->conn_no, atomic_read(&_cb->stats.send_msgs));
			//cb->stats.send_bytes += cb->send_sgl.length;
			//cb->stats.send_msgs++;
			atomic_set(&_cb->state, RDMA_SEND_COMPLETE);
			wake_up_interruptible(&_cb->sem); // Jack: added by Jack Should I add this? // TODO: take out and test
			break;

		case IB_WC_RDMA_WRITE:
			atomic_inc(&_cb->stats.write_msgs);
			EXP_LOG("<<<<< ----- from %d [[[[[ RDMA WRITE ]]]]] "
							"COMPLETION %d ----- (good) >>>>>\n",
							_cb->conn_no, atomic_read(&_cb->stats.write_msgs));
			atomic_set(&_cb->write_state, RDMA_WRITE_COMPLETE);
			wake_up_interruptible(&_cb->sem);
			break;

		case IB_WC_RDMA_READ:
			atomic_inc(&_cb->stats.read_msgs);
			EXP_LOG("<<<<< ----- from %d [[[[[ RDMA READ ]]]]] "
							"COMPLETION %d ----- (good) >>>>>\n",
							_cb->conn_no, atomic_read(&_cb->stats.read_msgs));
			atomic_set(&_cb->read_state, RDMA_READ_COMPLETE);
			wake_up_interruptible(&_cb->sem);
			break;

		case IB_WC_RECV:
			MSG_RDMA_PRK("Jack: ret %d recv_cnt %d\n", ret, ++recv_cnt);
			atomic_inc(&_cb->stats.recv_msgs);

			EXP_LOG("<<< --- from %d [[[[[ RECV ]]]]] COMPLETION %d --- >>>\n",
							  _cb->conn_no, atomic_read(&_cb->stats.recv_msgs));
			MSG_RDMA_PRK("< info > _wc->wr_id %p rw_t %d "
						 "r_recv_ticket %d r_rdma_ticket %d rdma_ack \"%s\"\n",
						 (void*)_wc->wr_id,
			 ((struct wc_struct*)_wc->wr_id)->element_addr->hdr.rw_ticket,
			 ((struct wc_struct*)_wc->wr_id)->element_addr->hdr.ticket,
			 ((struct wc_struct*)_wc->wr_id)->element_addr->hdr.rdma_ticket,
			 ((struct wc_struct*)_wc->wr_id)->element_addr->hdr.rdma_ack?"true":"false");

			ret = ib_kmsg_recv_long(_cb, (struct wc_struct*)_wc->wr_id);
			if (ret) {
				printk(KERN_ERR "< recv wc error: %d >\n", ret);
				goto error;
			}

			break;

		default:
			printk(KERN_ERR
				   "< %s:%d Unexpected opcode %d, Shutting down >\n",
				   __func__, __LINE__, _wc->opcode);
			goto error;
		}

		if(recv_cnt >= 10)
			break;
	}

	for(i=0; i<recv_cnt; i++) {
		struct ib_recv_wr *_rq_wr = jack_func(_cb->conn_no, true);
		ret = ib_post_recv(_cb->qp, _rq_wr, &bad_wr);
		if (ret) {
			printk(KERN_ERR "ib_post_recv failed: %d\n", ret);
			BUG();
		}
	}

	DEBUG_LOG("\n[[[[[external done]]]]] node %d\n\n", _cb->conn_no);
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

	DEBUG_LOG("\n->%s();\n", __func__);

	memset(&conn_param, 0, sizeof conn_param);
	conn_param.responder_resources = 1; // TODO: don't harcode
	conn_param.initiator_depth = 1; // TODO: don't harcode
	conn_param.retry_count = 10; // TODO: don't harcode

	ret = rdma_connect(cb->cm_id, &conn_param);
	if (ret) {
		printk(KERN_ERR "rdma_connect error %d\n", ret);
		return ret;
	}

	wait_event_interruptible(cb->sem,
						(int)atomic_read(&(cb->state)) >= CONNECTED);
	if ((int)atomic_read(&(cb->state)) == ERROR) {
		printk(KERN_ERR "wait for CONNECTED state %d\n",
										atomic_read(&(cb->state)));
		return -1;
	}

	DEBUG_LOG("rdma_connect successful\n");
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

	//sprintf(net_dev_name,"eth%d",ethernet_id);  // =strcat()
	device = __dev_get_by_name(&init_net, net_dev_name); // namespace=normale
	if(device) {
		*tmp_net_dev_name = (char *) vmalloc(sizeof(net_dev_name));
		memcpy(*tmp_net_dev_name, net_dev_name, sizeof(net_dev_name));
		in_dev = (struct in_device *)device->ip_ptr;
		if_info = in_dev->ifa_list;
		addr = (char *)&if_info->ifa_local;
		 DEBUG_LOG_V(KERN_WARNING "Device %s IP: %u.%u.%u.%u\n",
						net_dev_name,
						(__u32)addr[0],
						(__u32)addr[1],
						(__u32)addr[2],
						(__u32)addr[3]);
		return (addr[0]<<24 | addr[1]<<16 | addr[2]<<8 | addr[3]);
	}
	else{
		//sprintf(net_dev_name2,"eth%d",ethernet_id);
		device = __dev_get_by_name(&init_net, net_dev_name2);
		if(device) {
			*tmp_net_dev_name = (char *) vmalloc(sizeof(net_dev_name2));
			memcpy(*tmp_net_dev_name, net_dev_name2, sizeof(net_dev_name2));
			in_dev = (struct in_device *)device->ip_ptr;
			if_info = in_dev->ifa_list;
			addr = (char *)&if_info->ifa_local;
			DEBUG_LOG_V(KERN_WARNING "Device2 %s IP: %u.%u.%u.%u\n",
							net_dev_name2,
							(__u32)addr[0],
							(__u32)addr[1],
							(__u32)addr[2],
							(__u32)addr[3]);
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
			//cb[i]->port = htons(PORT+my_cpu);
			sin4->sin_port = _cb->port;
		}
		printk("client IP fillup _cb->addr %s _cb->port %d\n",
														_cb->addr, _cb->port);
	}
	else { // cb->server: load from global (ip=itself)
		if (cb[my_cpu]->addr_type == AF_INET) {
			struct sockaddr_in *sin4 = (struct sockaddr_in *)sin;
			sin4->sin_family = AF_INET;
			memcpy((void *)&sin4->sin_addr.s_addr, cb[my_cpu]->addr, 4);
			sin4->sin_port = cb[my_cpu]->port;
			printk("server IP fillup cb[my_cpu]->addr %s cb[my_cpu]->port %d\n",
											cb[my_cpu]->addr, cb[my_cpu]->port);
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

	DEBUG_LOG_V("%s(): IB_DEVICE_MEM_WINDOW %d support?%d\n",
				__func__, IB_DEVICE_MEM_WINDOW,
				device_attr.device_cap_flags&IB_DEVICE_MEM_WINDOW);
	DEBUG_LOG_V("%s(): IB_DEVICE_MEM_MGT_EXTENSIONS %d\n",
				__func__, IB_DEVICE_MEM_MGT_EXTENSIONS);
	DEBUG_LOG_V("%s(): IB_DEVICE_LOCAL_DMA_LKEY %d\n",
				__func__, IB_DEVICE_LOCAL_DMA_LKEY);
	DEBUG_LOG_V("%s(): (device_attr.device_cap_flags & needed_flags) %llx\n",
				__func__, (device_attr.device_cap_flags & needed_flags));

	if ((device_attr.device_cap_flags & needed_flags) != needed_flags) {
		printk(KERN_ERR
			"Fastreg not supported - device_cap_flags 0x%llx\n",
			(u64)device_attr.device_cap_flags);
		return 1; // still let it pass
	}
	DEBUG_LOG_V("Fastreg/local_dma_lkey supported - device_cap_flags 0x%llx\n",
											(u64)device_attr.device_cap_flags);
	return 1;
}

static int krping_bind_server(struct krping_cb *cb)
{
	struct sockaddr_storage sin;
	int ret;

	fill_sockaddr(&sin, cb);

	DEBUG_LOG("rdma_bind_addr\n");
	ret = rdma_bind_addr(cb->cm_id, (struct sockaddr *)&sin);
	if (ret) {
		printk(KERN_ERR "rdma_bind_addr error %d\n", ret);
		return ret;
	}

	DEBUG_LOG("rdma_listen\n");
	ret = rdma_listen(cb->cm_id, 99); // TODO: don't hardcode
	if (ret) {
		printk(KERN_ERR "rdma_listen failed: %d\n", ret);
		return ret;
	}

	return 0;
}

// TODO: cb -> _cb
static void krping_setup_wr(struct krping_cb *cb) // set up sgl, used for rdma
{
	int i=0, ret;
	DEBUG_LOG("\n\n\n->%s(): \n", __func__);
	DEBUG_LOG("@@@ 2 cb->recv_size = %d\n", cb->recv_size);
	for(i=0;i<MAX_RECV_WR;i++) {
		struct ib_recv_wr *bad_wr;
		struct ib_recv_wr *_rq_wr = jack_func(cb->conn_no, false);

		if(i<5 || i>(MAX_RECV_WR-5)) {
			DEBUG_LOG("_rq_wr %p cb->conn_no %d recv_size %d wr_id %p\n",
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

	DEBUG_LOG("@@@ <send addr>\n");
	DEBUG_LOG_V("@@@ 2 cb->send_sgl.addr = %p\n", (void*)cb->send_sgl.addr);	// this is not local_recv_buffer // it's exhanged local addr to remote
	DEBUG_LOG_V("@@@ 2 cb->send_dma_addr = %p\n", (void*)cb->send_dma_addr);	// user vaddr (O) mapped to the next line
	DEBUG_LOG_V("@@@ 2 cb->send_buf.payload = %p\n", cb->send_buf.payload);	 // kernel space buf (X) our msg
	DEBUG_LOG_V("@@@ 2 sizeof(cb->send_buf) = %ld\n", sizeof cb->send_buf);	 // kernel addr (X) (mapped to each other)
	DEBUG_LOG_V("@@@ 2 cb->recv_size = %d\n", cb->recv_size);				   // kernel addr (X)

	DEBUG_LOG_V("@@@ <lkey>\n");
	DEBUG_LOG_V("@@@ 2 cb->qp->device->local_dma_lkey = %d\n",
				cb->qp->device->local_dma_lkey);		//0
	DEBUG_LOG_V("@@@ 3lkey=%d from ../mad.c (ctx->pd->local_dma_lkey)\n",
							cb->pd->local_dma_lkey);	//4450 (dynamic diff)
	DEBUG_LOG_V("@@@ 4lkey=%d from client/server example(cb->mr->lkey)\n",
							cb->reg_mr->lkey);		  //4463 (dynamic diff)

	cb->sq_wr.opcode = IB_WR_SEND;			  // normal send / recv
	cb->sq_wr.send_flags = IB_SEND_SIGNALED;	// ?
	cb->sq_wr.sg_list = &cb->send_sgl;		  // sge
	cb->sq_wr.num_sge = 1;

	DEBUG_LOG_V("anoter rdma buffer (passive buffer)\n");
	//active: rdma_dma_addr; passive: passive_dma_addr
	// READ/WRITE passive buf
	cb->rdma_sgl.addr = cb->passive_dma_addr;

	cb->rdma_sq_wr.wr.sg_list = &cb->rdma_sgl;
	cb->rdma_sq_wr.wr.send_flags = IB_SEND_SIGNALED;
	cb->rdma_sq_wr.wr.num_sge = 1;

	/*
	 * A chain of 2 WRs, INVALDATE_MR + REG_MR.
	 * both unsignaled.  The client uses them to reregister
	 * the rdma buffers with a new key each iteration.
	 */
	cb->reg_mr_wr.wr.opcode = IB_WR_REG_MR;		 //(legacy:fastreg)
	cb->reg_mr_wr.mr = cb->reg_mr;

	cb->reg_mr_wr_passive.wr.opcode = IB_WR_REG_MR; //(legacy:fastreg)
	cb->reg_mr_wr_passive.mr = cb->reg_mr_passive;

	cb->invalidate_wr.next = &cb->reg_mr_wr.wr;	 //
	cb->invalidate_wr.opcode = IB_WR_LOCAL_INV;	 // invalidate Memory Window

	cb->invalidate_wr_passive.next = &cb->reg_mr_wr_passive.wr; //
	cb->invalidate_wr_passive.opcode = IB_WR_LOCAL_INV; // invalidate Memory Window
	/*  The reg mem_mode uses a reg mr on the client side for the
	 *  passive_buf and rdma_buf buffers.  Each time the client will advertise
	 *  one of these buffers, it invalidates the previous registration and fast
	 *  registers the new buffer with a new key.
	 *
	 *  If the server_invalidate
	 *  option is on, then the server will do the invalidation via the
	 * "go ahead" messages using the IB_WR_SEND_WITH_INV opcode.Otherwise the
	 * client invalidates the mr using the IB_WR_LOCAL_INV work request.
	 */
	return;
}

static int krping_setup_qp(struct krping_cb *cb, struct rdma_cm_id *cm_id)
{
	int ret;
	struct ib_cq_init_attr attr = {0};

	DEBUG_LOG("\n->%s();\n", __func__);

	//cb->pd = ib_alloc_pd(cm_id->device, 0);
	cb->pd = ib_alloc_pd(cm_id->device);
	if (IS_ERR(cb->pd)) {
		printk(KERN_ERR "ib_alloc_pd failed\n");
		return PTR_ERR(cb->pd);
	}
	DEBUG_LOG("created pd %p\n", cb->pd);

	attr.cqe = cb->txdepth * 8;
	attr.comp_vector = 0; // int mask
	cb->cq =
		ib_create_cq(cm_id->device, krping_cq_event_handler, NULL, cb, &attr);
	if (IS_ERR(cb->cq)) {
		printk(KERN_ERR "ib_create_cq failed\n");
		ret = PTR_ERR(cb->cq);
		goto err1;
	}
	DEBUG_LOG("created cq %p task\n", cb->cq);

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
	DEBUG_LOG("created qp %p\n", cb->qp);
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
	DEBUG_LOG("\n->%s();\n", __func__);
	DEBUG_LOG("krping_setup_buffers called on cb %p\n", cb);

	/*
	cb->recv_dma_addr = dma_map_single(cb->pd->device->dma_device,	  // for remote access  (mapping together)
						&cb->recv_buf[0],							   // for local access (mapping together) // suppose continue
						cb->recv_size * MAX_RECV_WR, DMA_BIDIRECTIONAL);
	pci_unmap_addr_set(cb, recv_mapping, cb->recv_dma_addr);			// == cb->recv_mapping = cb->recv_dma_addr

	printk("%s(): recv_mapping %p (store it and release it) <- "
		   "cb->recv_dma_addr %p \n", __func__,
			(void*)cb->recv_mapping, (void*)cb->recv_dma_addr);
	printk("%s(): sizeof(&cb->recv_buf[0] %lu (X) "
		   "sizeof(cb->recv_buf)*MAX_RECV_WR %lu (O)\n",__func__,
			sizeof(&cb->recv_buf[0]), sizeof(cb->recv_buf)*MAX_RECV_WR);
	printk("%s(): sizeof(cb->recv_buf[0]*MAX_RECV_WR %lu "
		   "cb->recv_size*MAX_RECV_WR %d\n",__func__,
			sizeof(cb->recv_buf[0])*MAX_RECV_WR, cb->recv_size*MAX_RECV_WR);
	*/

	cb->send_dma_addr = dma_map_single(cb->pd->device->dma_device,  // send
							   &cb->send_buf, sizeof(cb->send_buf), // use send buffer
							   DMA_BIDIRECTIONAL);				  // cb->send_dma
	pci_unmap_addr_set(cb, send_mapping, cb->send_dma_addr);

	/* active rw */
	//cb->rdma_buf = kmalloc(cb->rdma_size, GFP_DMA);	   // (X) GFP_DMA
	cb->rdma_buf = kmalloc(cb->rdma_size, GFP_KERNEL);	  // (O) alloc rdma buffer (the only allocated buf)
	if (!cb->rdma_buf) {
		DEBUG_LOG("rdma_buf malloc failed\n");
		ret = -ENOMEM;
		goto bail;
	}
	cb->rdma_dma_addr = dma_map_single(cb->pd->device->dma_device,  //
						   cb->rdma_buf, cb->rdma_size,			 // user dma buffer
						   DMA_BIDIRECTIONAL);					  //
	pci_unmap_addr_set(cb, rdma_mapping, cb->rdma_dma_addr);

	cb->page_list_len = (((cb->rdma_size - 1) & PAGE_MASK) + PAGE_SIZE)
															>> PAGE_SHIFT;
	/* mr for active */
	cb->reg_mr = ib_alloc_mr(cb->pd, IB_MR_TYPE_MEM_REG,			// fill up lkey and rkey
										 cb->page_list_len);
	/* mr for passive */
	cb->reg_mr_passive = ib_alloc_mr(cb->pd, IB_MR_TYPE_MEM_REG,	// fill up lkey and rkey
										 cb->page_list_len);

	if (IS_ERR(cb->reg_mr)) {
		ret = PTR_ERR(cb->reg_mr);
		DEBUG_LOG("reg_mr failed %d\n", ret);
		goto bail;
	}
	if (IS_ERR(cb->reg_mr_passive)) {
		ret = PTR_ERR(cb->reg_mr_passive);
		DEBUG_LOG("reg_mr_passive failed %d\n", ret);
		goto bail;
	}

	DEBUG_LOG("@@@ 1 cb->rdma_buf = dma_map_single( cb->rdma_buf ) = "
							"0x%p kmalloc (mapping together)\n", cb->rdma_buf);
	DEBUG_LOG("@@@ 1 cb->rdma_dma_addr = 0x%p a kernel vaddr for remote "
					"access  (mapping together)\n", (void*)cb->rdma_dma_addr);
	DEBUG_LOG_V("\n@@@ after mr\n");
	DEBUG_LOG_V("@@@ reg rkey %d page_list_len %u\n",
										cb->reg_mr->rkey, cb->page_list_len);
	DEBUG_LOG_V("@@@ 1 Jack cb->reg_mr->lkey %d from mr \n", cb->reg_mr->lkey);
	//DEBUG_LOG("@@@ 1 cb->send_sgl.lkey %d from mr \n", cb->send_sgl.lkey); //0
	//DEBUG_LOG("@@@ 1 cb->recv_sgl.lkey %d from mr \n", cb->recv_sgl.lkey); //0
	DEBUG_LOG_V("@@@ 1 correct lkey=%d (ref: ./drivers/infiniband/core/mad.c )"
				"(ctx->pd->local_dma_lkey)\n", cb->pd->local_dma_lkey); //4xxx dynamic
	/*
		// these are all NULL.
		cb->dma_mr->lkey, cb->dma_mr->rkey	  // null
		cb->rdma_mr->lkey, cb->rdma_mr->rkey	// null
		cb->start_mr->lkey, cb->start_mr->rkey  // null
	*/

	/* passive rw */
	DEBUG_LOG("only client(not true) setup passive_buf passive_dma_addr\n");
	cb->passive_buf = kmalloc(cb->rdma_size, GFP_KERNEL);
	if (!cb->passive_buf) {
		DEBUG_LOG("passive_buf malloc failed\n");
		ret = -ENOMEM;
		goto bail;
	}
	cb->passive_dma_addr = dma_map_single(cb->pd->device->dma_device,
					   cb->passive_buf, cb->rdma_size,
					   DMA_BIDIRECTIONAL);
	pci_unmap_addr_set(cb, start_mapping, cb->passive_dma_addr);
	DEBUG_LOG("@@@ cb->passive_dma_addr = 0x%p Jack (only client->not true)\n",
												(void*)cb->passive_dma_addr);

	krping_setup_wr(cb); //(only server setup rdma_sq_wr since only server issue rdma operations)
	DEBUG_LOG("allocated & registered buffers done!\n");
	DEBUG_LOG("\n\n");
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
	if (cb->rdma_buf)
		kfree(cb->rdma_buf);
	if (cb->passive_buf)
		kfree(cb->passive_buf);
	return ret;
}


static int krping_accept(struct krping_cb *cb)
{
	struct rdma_conn_param conn_param;
	int ret;
	DEBUG_LOG("\n->%s(); cb->conn_%d accepting client connection request....\n",
														__func__, cb->conn_no);
	memset(&conn_param, 0, sizeof conn_param);
	conn_param.responder_resources = 1;
	conn_param.initiator_depth = 1;

	ret = rdma_accept(cb->child_cm_id, &conn_param);
	if (ret) {
		printk(KERN_ERR "rdma_accept error: %d\n", ret);
		return ret;
	}

		DEBUG_LOG("%s(): wating for a signal...............\n", __func__);
		//wait_event(cb->sem, atomic_read(&(cb->state)) >= CONNECTED); // working
		wait_event_interruptible(cb->sem,
					atomic_read(&(cb->state)) >= CONNECTED); // testing now
													// have a look child_cm_id
		DEBUG_LOG("%s(): got the signal !!!!(GOOD)!!!!!!! cb->state = %d \n",
										__func__, atomic_read(&(cb->state)));
		if (atomic_read(&(cb->state)) == ERROR) {
			printk(KERN_ERR "wait for CONNECTED state %d\n",
												atomic_read(&(cb->state)));
			return -1;
		}


	is_connection_done[cb->conn_no] = 1;
	smp_mb(); // since my_cpu is externed (global)
	DEBUG_LOG("acception done!\n");
	return 0;
}


static void krping_free_buffers(struct krping_cb *cb)
{
	DEBUG_LOG("krping_free_buffers called on cb %p\n", cb);

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
	kfree(cb->rdma_buf);
	if (cb->passive_buf) {
		dma_unmap_single(cb->pd->device->dma_device,
			 pci_unmap_addr(cb, start_mapping),
			 cb->rdma_size, DMA_BIDIRECTIONAL);
		kfree(cb->passive_buf);
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

	DEBUG_LOG("--thread--> %s(): conn %d\n", __func__, cb->conn_no);
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

	is_connection_done[cb->conn_no] = 1; // atomic is more safe
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

	DEBUG_LOG("<<< %s(): cb->conno %d >>>\n", __func__, listening_cb->conn_no);
	DEBUG_LOG("<<< %s(): cb->conno %d >>>\n", __func__, listening_cb->conn_no);
	DEBUG_LOG("<<< %s(): cb->conno %d >>>\n", __func__, listening_cb->conn_no);

	ret = krping_bind_server(listening_cb);
	if (ret)
		return ret;

	DEBUG_LOG("\n\n\n");

	//TODO: MCJack-modify outside
	//- create multiple connections -//
	while(1){
		/* Wait for client's Start STAG/TO/Len */
		msleep(1000);
		wait_event_interruptible(listening_cb->sem,
					atomic_read(&(listening_cb->state)) == CONNECT_REQUEST); // krping_cma_event_handler
		if (atomic_read(&(listening_cb->state)) != CONNECT_REQUEST) {
			printk(KERN_ERR "wait for CONNECT_REQUEST state %d\n",
										atomic_read(&(listening_cb->state)));
			continue;
		}
		printk("got a connection (Jack)\n");

		/* create a thread for this */
		//exec_thread_data *exec_data = (exec_thread_data *) kmalloc(sizeof(exec_thread_data), GFP_KERNEL);
		//exec_data->conn_no = conn_no;
		//TODO: catch return thread
		//struct krping_cb* cb = (struct krping_cb*) *(cb + (sizeof(struct krping_cb)*(my_cpu)) + (sizeof(struct krping_cb)*(i)));
		//_cb = cb[my_cpu];
		_cb = cb[my_cpu+i];
		_cb->server=1;

		printk("1 _cb->conn_no %d\n", _cb->conn_no);
		printk("2 cb[my_cpu] %p cb[my_cpu]->child_cm_id %p\n",
										cb[my_cpu], cb[my_cpu]->child_cm_id);
		printk("2 cb[my_cpu+i] %p cb[my_cpu+i]->child_cm_id %p\n",
									cb[my_cpu+i], cb[my_cpu+i]->child_cm_id);

		//_cb->child_cm_id->context = _cb; //TODO: none-sense? for int handler to retrive cb?

		printk("3 _cb->child_cm_id %p = cb_listen->child_cm_id %p "
								"(Jack!!!!!!1/31)\n",
								_cb->child_cm_id, cb_listen->child_cm_id);

		_cb->child_cm_id = cb_listen->child_cm_id; // will be used [setup_qp(SRWRirq)] -> setup_buf ->

		printk("3 _cb->child_cm_id %p = cb_listen->child_cm_id %p "
								"(Jack!!!!!!1/31)\n",
								_cb->child_cm_id, cb_listen->child_cm_id);
		t = kthread_run(krping_persistent_server_thread, _cb,
									"krping_persistent_server_conn_thread");
		BUG_ON(IS_ERR(t));

		atomic_set(&listening_cb->state, IDLE);
		i++;
	}
	// program done
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
								atomic_read(&(cb->state)) >= ROUTE_RESOLVED);
	if (atomic_read(&(cb->state)) != ROUTE_RESOLVED) {
		printk(KERN_ERR "addr/route resolution did not resolve: state %d\n",
													atomic_read(&(cb->state)));
		return -EINTR;
	}

	if (!reg_supported(cb->cm_id->device)) //Jack
		return -EINVAL;

	DEBUG_LOG("rdma_resolve_addr - rdma_resolve_route successful\n");
	return 0;
}

static int krping_create_qp(struct krping_cb *cb)
{
	struct ib_qp_init_attr init_attr;
	int ret;

	memset(&init_attr, 0, sizeof(init_attr));
	init_attr.cap.max_send_wr = cb->txdepth;
	init_attr.cap.max_recv_wr = MAX_RECV_WR*2;	// TODO: +++++++++++++++++++

	/* For flush_qp() */
	init_attr.cap.max_send_wr++;		// TODO: +++++++++++++++++++++++++++++
	init_attr.cap.max_recv_wr++;		// TODO: +++++++++++++++++++++++++++++

	init_attr.cap.max_recv_sge = 1;	 // ok for now
	init_attr.cap.max_send_sge = 1;	 // ok for now
	init_attr.qp_type = IB_QPT_RC;
	init_attr.send_cq = cb->cq;		 // TODO: +++++++++++++++++++++++++++++
	init_attr.recv_cq = cb->cq;		 // TODO: +++++++++++++++++++++++++++++
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

/* action for bottom half
 * handler no longer has to kfree the lmsg !!
 */
static void pcn_kmsg_handler_BottomHalf(struct work_struct * work)
{
	DEBUG_LOG("%s(): \n", __func__);
	pcn_kmsg_work_t *w = (pcn_kmsg_work_t *) work;
	struct pcn_kmsg_long_message *lmsg =  &w->lmsg;
	pcn_kmsg_cbftn ftn;	 // function pointer - typedef int (*pcn_kmsg_cbftn)(struct pcn_kmsg_message *);
	if( lmsg->hdr.type < 0 || lmsg->hdr.type >= PCN_KMSG_TYPE_MAX) {
		DEBUG_LOG(KERN_INFO "Received invalid message type %d > MAX %d\n",
										lmsg->hdr.type, PCN_KMSG_TYPE_MAX);
	}
	else {
		printk(" Grabing to callbacks kwq->lmsg->hdr.type %d %s "
							"kwq->lmsg->hdr.size %d rw_t %d\n",
							lmsg->hdr.type,
							lmsg->hdr.type==2?"REQUEST":"RESPONSE",
							lmsg->hdr.size,
							lmsg->hdr.rw_ticket);

		/* normal msg */
		ftn = callbacks[lmsg->hdr.type];
		if(ftn != NULL) {
			ftn((void*)lmsg); // Jack: invoke callback with input arguments // callback has to kfree the buffer
		} else {
			DEBUG_LOG(KERN_INFO "Recieved message type %d size %d "
											"has no registered callback!\n",
											lmsg->hdr.type, lmsg->hdr.size);
			BUG_ON(-1);
		}
	}
	DEBUG_LOG("%s(): done & free everything\n\n", __func__);
	//kfree((void*)lmsg); // static, so, no need
	kfree((void*)w);
	//TODO: should I alloc lmsg as a static way, and free here?
	return;
}

/*
 * parse recved msg in the buf to msg_layer
 * in INT
 */
//static int ib_kmsg_recv_long(struct krping_cb *cb, struct ib_wc *wc)
static int ib_kmsg_recv_long(struct krping_cb *cb,
							 struct wc_struct *wcs)
{
	struct pcn_kmsg_long_message *lmsg = wcs->element_addr;
	//DEBUG_LOG("%s():\n", __func__);
	// check msg healthy
	//if (wc->byte_len != sizeof(cb->recv_buf)) {

	/*
	if (wc->byte_len != cb->recv_size) {
		printk(KERN_ERR "Received bogus data, size %d\n", wc->byte_len);
		return -1;
	}*/

	// ib (data in cb->recv_buf)
	//----------------------------------------------------------
	// pcn_msg (abstraction msg layer)

	// got msg form ib, attach to msg_layer.
	// !! don't transfer to user defined type. that should be done in user's handler

	/*
		// example code - directly process
		remote_thread_first_test_request_t* request = (remote_thread_first_test_request_t*) &cb->recv_buf; // int buf
		printk("%s(): request->hdr.from_nid %d\n", __func__, request->hdr.from_nid);
		printk("urlmsg(u def)->example1 %d IF SEE 1 PREPARE GO HOME\n", request->example1);
		printk("urlmsg(u def)->example2 %d IF SEE 2 PREPARE GO HOME\n", request->example2);
		printk("urlmsg(u def)->msg %d IF SEE morethan 4096 PREPARE GO HOME\n", sizeof(request->msg));
		printk("urlmsg(u def)->msg %s IF SEE last 10 char PREPARE GO HOME\n", request->msg+sizeof(request->msg)-10);
	*/

	// (struct pcn_kmsg_buf *buf(wrraper), int conn_no)
	// -------bottom half (deq) --------
	// (struct pcn_kmsg_buf *buf(wrraper), int conn_no)

	// - alloc & cpy msg to kernel buffer
	//struct pcn_kmsg_long_message *lmsg = (struct pcn_kmsg_long_message*) pcn_kmsg_alloc_msg_long(cb->recv_buf[0].hdr.size);
		/////////////////////////////////////////////////////HOW can i know??????????????????????????????????????

	//if(unlikely(!memcpy(lmsg, &cb->recv_buf[wc->wr_id], cb->recv_buf[wc->wr_id].hdr.size))) /////////// kill me
	//	BUG_ON(-1); // kill me
	// can just use the size since we assum fix size

	// - semaphore down empty!

	// - semaphore up full!

	// - since interrupt triggers different thread and data set one by one? we don't have to put a lock here

	// - dispatch to workers (like enq_()) to do callback

	// - compose arguments
	/*
	EXP_LOG("%s(): producing BottomHalf wc->wr_id %llu hdr.size %d\n",
						__func__, wc->wr_id, cb->recv_buf[wc->wr_id-100].hdr.size);
	*/
	EXP_LOG("%s(): producing BottomHalf wc->wr_id = lmsg %p hdr.size %d\n",
						__func__, (void*)lmsg, lmsg->hdr.size);

	pcn_kmsg_work_t *kmsg_work = kmalloc(sizeof(pcn_kmsg_work_t), GFP_ATOMIC);
	if (likely(kmsg_work)) {
		// wrap the entire msg
		//kmsg_work->lmsg = lmsg;

		MSG_RDMA_PRK("bf: Spwning BottomHalf, leaving INT kwq->lmsg->hdr.type %d %s "
							"kwq->lmsg->hdr.size %d rw_t %d\n",
							lmsg->hdr.type,
							lmsg->hdr.type==2?"REQUEST":"RESPONSE",
							lmsg->hdr.size,
							lmsg->hdr.rw_ticket );

		if(unlikely(!memcpy(&kmsg_work->lmsg, lmsg, lmsg->hdr.size))) { // including hdr
										//sizeof(struct pcn_kmsg_long_message)))) { //
			BUG_ON(-1);
		}
		// remember to case. Jack: TODO ask why this is correct (from larger->smaller ---send---> smaller->larger)
#if MSGDEBUG
//		kmsg_work->lmsg.hdr.ticket = atomic_inc_return(&g_recv_ticket); // from 1
//		DEBUG_LOG("%s() recv ticket %d\n", __func__, kmsg_work->lmsg.hdr.ticket);
#endif
		MSG_RDMA_PRK("af: Spwning BottomHalf, leaving INT kwq->lmsg->hdr.type %d %s "
							"kwq->lmsg->hdr.size %d rw_t %d\n",
							kmsg_work->lmsg.hdr.type,
							kmsg_work->lmsg.hdr.type==2?"REQUEST":"RESPONSE",
							kmsg_work->lmsg.hdr.size,
							kmsg_work->lmsg.hdr.rw_ticket );

		if(unlikely(kmsg_work->lmsg.hdr.rw_ticket != lmsg->hdr.rw_ticket)) {
			printk("ERROR: Jack skips\n");
			BUG();
			kfree(kmsg_work);
			return 0;
		}
		//pcn_kmsg_handler_BottomHalf(kmsg_work);
		INIT_WORK((struct work_struct *)kmsg_work, pcn_kmsg_handler_BottomHalf);	// event
		if(unlikely(!queue_work(msg_handler, (struct work_struct *)kmsg_work)))	 // wq
		   BUG();

	} else {
		printk("Failed to kmalloc work structure!\n");
		BUG_ON(-1);
	}

	// TODO
	kfree(lmsg);
	kfree(wcs->recv_sgl);
	kfree(wcs->rq_wr);
	kfree(wcs);
	return 0;
}

static int krping_run_client(struct krping_cb *cb)
{
	int ret;
	//struct ib_recv_wr *bad_wr;

	DEBUG_LOG("<<<<<<<< %s(): cb->conno %d >>>>>>>>\n", __func__, cb->conn_no);
	DEBUG_LOG("<<<<<<<< %s(): cb->conno %d >>>>>>>>\n", __func__, cb->conn_no);
	DEBUG_LOG("<<<<<<<< %s(): cb->conno %d >>>>>>>>\n", __func__, cb->conn_no);
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
	DEBUG_LOG("ib_post_recv(manually)<<<<\n");
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
										struct pcn_kmsg_rdma_message* inc_lmsg)
{
	remote_thread_rdma_read_request_t* response =
								(remote_thread_rdma_read_request_t*) inc_lmsg;
	struct krping_cb *_cb = cb[response->hdr.from_nid]; // select the correct cb

	// example
	//response->hdr.rdma_size;	  // if send/recv, this = 0
	//response->hdr.rdma_ack;	   // activator: 1 passive: 0

	// DBG for READ - show the data remote side should see
	// printk out the msg info remote side should know
	//TODO: NO NEED  rdma_buf -> passive_buf, because this is just a ack showing what should be seen in the passive(remote) side
	// active side provide original message
	EXP_LOG("%s(): response->hdr.rdma_size %d _cb->rdma_buf(first10)%.10s "
									"rdma_ack %s(==true)\n",
									__func__, response->hdr.rdma_size,
									_cb->rdma_buf+response->hdr.rdma_size-10,
									response->hdr.rdma_ack?"true":"false");

	EXP_LOG("response->hdr.remote_rkey %u remote_addr %p rdma_size %u "
					"rw_t %d recv_ticket %d ack_rdma_ticket %d\n",
					response->hdr.remote_rkey,
					(void*)response->hdr.remote_addr,
					response->hdr.rdma_size,
					response->hdr.rw_ticket, // dbg
					response->hdr.ticket, // send/recv dbg
					response->hdr.rdma_ticket); //rdma dbg


	MSG_SYNC_PRK("///////READ active unlock() %d rw_t %d conn %d///////////\n",
										(int)atomic_read(&_cb->active_cnt),
										response->hdr.rw_ticket, // rdms dbg (sol to sync P)
										_cb->conn_no);
	mutex_unlock(&_cb->active_mutex);

	DEBUG_LOG("%s(): end\n", __func__);
	return;
}

static void handle_remote_thread_rdma_write_response(
										struct pcn_kmsg_rdma_message* inc_lmsg)
{
	remote_thread_rdma_write_request_t* response =
								(remote_thread_rdma_write_request_t*) inc_lmsg;
	struct krping_cb *_cb = cb[response->hdr.from_nid]; // select the correct cb

	// examples
	// response->hdr.rdma_size;	 // if send/recv, this = 0
	// response->hdr.rdma_ack;	  // activator: 1 passive: 0

	// Check WRITE result (dbg)
	EXP_LOG("%s(): response->hdr.rdma_size %d _cb->passive_buf(first10)%.10s "
								"rdma_ack %s(==true)\n",
								__func__, response->hdr.rdma_size,
								_cb->passive_buf+response->hdr.rdma_size-10,
								response->hdr.rdma_ack?"true":"false");
	EXP_LOG("response->hdr.remote_rkey %u remote_addr %p rdma_size %u "
								"rw_t %d ticket %d rdma_ticket %d\n",
								response->hdr.remote_rkey,
								(void*)response->hdr.remote_addr,
								response->hdr.rdma_size,
								response->hdr.rw_ticket,
								response->hdr.ticket,
								response->hdr.rdma_ticket);

	/*
	// check - an example (for write)
	//memcmp(g_test_buf, _cb->rdma_buf, response->hdr.rdma_size);
	if(memcmp(g_test_buf, _cb->rdma_buf, g_test_remote_len)) { // fix upper one
		printk("%s(): RDMA read data the same", __func__, );
	}
	else {
		printk("%s(): RDMA read data the same", __func__, );
	}
	//clean test buf ()
	//memset(_cb->rdma_buf, 0,1);
	*/

	MSG_SYNC_PRK("/////////////WRITE active unlock() %d////////////////\n",
											(int)atomic_read(&_cb->active_cnt));
	mutex_unlock(&_cb->active_mutex);

	DEBUG_LOG("%s(): end\n\n\n", __func__);
	return;
}


// Initialize callback table to null, set up control and data channels
int __init initialize()
{
	int i, err, conn_no;
	char *tmp_net_dev_name=NULL;
	struct task_struct *t;
	// TODO: check how to assign a priority to these threads! make msg_layer faster (higher prio)
	// struct sched_param param = {.sched_priority = 10};
	KRPRINT_INIT("--- Popcorn messaging layer init starts ---\n");

	KRPRINT_INIT("Registering softirq handler (BottomHalf)...\n");
	//open_softirq(PCN_KMSG_SOFTIRQ, pcn_kmsg_action); // TODO: check open_softirq()
	msg_handler = create_workqueue("MSGHandBotm"); // per-cpu

	for(i=0; i<MAX_NUM_NODES; i++) {
		// find my name
		if ( get_host_ip(&tmp_net_dev_name) == ip_table2[i])  {
			my_cpu=i;
			KRPRINT_INIT("Device \"%s\" my_cpu=%d on machine IP %u.%u.%u.%u\n",
												tmp_net_dev_name, my_cpu,
												(ip_table2[i]>>24)&0x000000ff,
												(ip_table2[i]>>16)&0x000000ff,
												(ip_table2[i]>> 8)&0x000000ff,
												(ip_table2[i]>> 0)&0x000000ff);
			vfree(tmp_net_dev_name);
			break;
		}
	}
	if(my_cpu==-99)
		BUG_ON("my_cpu isn't initialized\n");

	smp_mb(); // since my_cpu is extern (global)
	KRPRINT_INIT("---------------------------------------------------------\n");
	KRPRINT_INIT("---- updating to my_cpu=%d wait for a moment ----\n", my_cpu);
	KRPRINT_INIT("---------------------------------------------------------\n");
	KRPRINT_INIT("MSG_LAYER: Initialization my_cpu=%d\n", my_cpu);

	for (i = 0; i<MAX_NUM_NODES; i++) {
		is_connection_done[i]=PCN_CONN_WATING;
	}

	/* Initilaize the IB -
	 * Each node has a connection table like tihs:
	 * -----------------------------------------------------------------------------
	 * | connect | connect | (many)... | my_cpu(one) | accept | accept | (many)... |
	 * -----------------------------------------------------------------------------
	 * my_cpu:  no need to talk to itself
	 * connect: connecting to existing nodes
	 * accept:  waiting for the connection requests from later nodes
	 */
	for (i=0; i<MAX_NUM_NODES; i++) {
		DEBUG_LOG_V("cb[%d] %p\n", i, cb[i]);

		conn_no=i;
		// 0. create and save to list
		//cb[i] = kzalloc(sizeof(*cb), GFP_KERNEL); // TODO: 1. test 2. release
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
		cb[i]->addr_type = AF_INET;							 // [IPv4]/V6 // for determining
		cb[i]->port = htons(PORT);							  // sock always the same port, not for ib
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
	 * -----------------------------------------------------------------------------
	 * | connect | connect | (many)... | my_cpu(one) | accept | accept | (many)... |
	 * -----------------------------------------------------------------------------
	 * my_cpu:  no need to talk to itself
	 * connect: connecting to existing nodes
	 * accept:  waiting for the connection requests from later nodes
	 */

	//- One persistent listening server -//
	cb_listen = cb[my_cpu];
	cb_listen->server = 1;
	t = kthread_run(krping_run_server, cb_listen,
					"krping_persistent_server_listen_thread"); // 1. [my_cpu(accept)], connect
	BUG_ON(IS_ERR(t));

	is_connection_done[my_cpu] = 1;

	//- client -//
	for (i=0; i<MAX_NUM_NODES; i++) { // TODO: May this order makes a DEADLOCK??
		if (i==my_cpu) {
			continue; // has done (server)
		}

		conn_no = i;	 // Take node 1 for example. connect0 [1] accept2
		if (conn_no < my_cpu) { // 1. my_cpu(accept), [connect]
			cb[conn_no]->server = 0;

			// server/client dependant init
			msleep(1000);  // TODO: replace this with other things
			err = krping_run_client(cb[conn_no]); // connect_to()
			if (err) {
				printk("WRONG!!\n");
				return err;
			}

			is_connection_done[conn_no] = 1; //Jack: atomic is more safe
			smp_mb(); // Jack: just call it one time in the end, it should be fine
		}
		else{
			DEBUG_LOG("no action needed for conn %d "
					   "(listening will take care)\n", i);
		}
		// TODO: Jack: use kthread run?
		//sched_setscheduler(<kthread_run()'s return>, SCHED_FIFO, &param);
		//set_cpus_allowed_ptr(<kthread_run()'s return>, cpumask_of(i%NR_CPUS));
	}

	for ( i=0; i<MAX_NUM_NODES; i++ ) {
		while ( is_connection_done[i]==PCN_CONN_WATING ) { //Jack: atomic is more safe
			DEBUG_LOG("waiting for is_connection_done[%d]\n", i);
			msleep(3000);
		}
	}

	// load
	send_callback = (send_cbftn)ib_kmsg_send_long;  // send()
	send_callback2 = (send_cbftn)ib_kmsg_send_rdma; // rdma read/write()
	DEBUG_LOG("Value of send ptr = %p\n", send_callback);
	DEBUG_LOG("--- Popcorn messaging layer is up ---\n");


	// TODO: move to an earlier place
	DEBUG_LOG("--- init all ib[]->state ---\n");
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

	/* register RDMA callbacks */
	pcn_kmsg_register_callback((enum pcn_kmsg_type)PCN_KMSG_TYPE_RDMA_READ_REQUEST,	// ping - rdma w/r must need
					(pcn_kmsg_cbftn)handle_remote_thread_rdma_read_request);
	pcn_kmsg_register_callback((enum pcn_kmsg_type)PCN_KMSG_TYPE_RDMA_READ_RESPONSE,	// pong -
					(pcn_kmsg_cbftn)handle_remote_thread_rdma_read_response);

	pcn_kmsg_register_callback((enum pcn_kmsg_type)PCN_KMSG_TYPE_RDMA_WRITE_REQUEST,	// ping - rdma w/r must need
					(pcn_kmsg_cbftn)handle_remote_thread_rdma_write_request);
	pcn_kmsg_register_callback((enum pcn_kmsg_type)PCN_KMSG_TYPE_RDMA_WRITE_RESPONSE,	// pong -
					(pcn_kmsg_cbftn)handle_remote_thread_rdma_write_response);

	smp_mb();
	DEBUG_LOG(KERN_INFO "Popcorn Messaging Layer Initialized\n"
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

	// reg_mr = ib_reg_mr(pd, buf, size, IBV_ACCESS_LOCAL_WRITE);
	// cb->reg_mr = ib_alloc_mr(cb->pd, IB_MR_TYPE_MEM_REG, // fill up lkey and rkey
	//													  cb->page_list_len);
	// old key
	_cb->invalidate_wr.ex.invalidate_rkey = _cb->reg_mr->rkey; // save corrent reg rkey

	//
	// Update the reg key.
	//
	//ib_update_fast_reg_key(cb->reg_mr, ++cb->key);
	ib_update_fast_reg_key(_cb->reg_mr, _cb->key); // Jack: keeps the key the same
	_cb->reg_mr_wr.key = _cb->reg_mr->rkey;

	//
	// Setup permissions
	//
	// if (buf == (u64)cb->passive_dma_addr || buf == (u64)cb->rdma_dma_addr) {
	// reg_mr_wr_passive in another function
	_cb->reg_mr_wr.access = IB_ACCESS_REMOTE_READ   |
							IB_ACCESS_REMOTE_WRITE  |
							IB_ACCESS_LOCAL_WRITE   |
							IB_ACCESS_REMOTE_ATOMIC; // unsafe but works

	sg_dma_address(&sg) = buf;		  // passed by caller
	sg_dma_len(&sg) = rdma_len;		 // support "dynamically" chaging  R/W length !!!!!
	EXP_LOG("%s(): rdma_len (dynamical) %d\n", __func__, sg_dma_len(&sg));

	//ret = ib_map_mr_sg(cb->reg_mr, &sg, 1, NULL, PAGE_SIZE);
	ret = ib_map_mr_sg(_cb->reg_mr, &sg, 1, PAGE_SIZE);  // snyc ib_dma_sync_single_for_cpu/dev
	BUG_ON(ret <= 0 || ret > _cb->page_list_len);

	EXP_LOG("%s(): ### post_inv = %d, reg_mr new rkey %d pgsz %u len %u"
			" rdma_len (dynamical) %d iova_start %llx\n", __func__, post_inv,
			_cb->reg_mr_wr.key, _cb->reg_mr->page_size, _cb->reg_mr->length,
			rdma_len, _cb->reg_mr->iova);

	mutex_lock(&_cb->qp_mutex);
	if (likely(post_inv)) // becaus remote doesn't have inv, so manual? then W?
		ret = ib_post_send(_cb->qp, &_cb->invalidate_wr, &bad_wr); //
	else
		; //ret = ib_post_send(_cb->qp, &_cb->reg_mr_wr.wr, &bad_wr); // by passive WRITE in krping.c
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
	_cb->invalidate_wr_passive.ex.invalidate_rkey = _cb->reg_mr_passive->rkey; // save corrent reg rkey

	// Update the reg key.
	ib_update_fast_reg_key(_cb->reg_mr_passive, _cb->key); // Jack: keeps the key the same
	_cb->reg_mr_wr_passive.key = _cb->reg_mr_passive->rkey;

	_cb->reg_mr_wr_passive.access = IB_ACCESS_REMOTE_READ   |
									IB_ACCESS_REMOTE_WRITE  |
									IB_ACCESS_LOCAL_WRITE   |
									IB_ACCESS_REMOTE_ATOMIC; // unsafe but works

	sg_dma_address(&sg) = buf;		  // passed by caller
	sg_dma_len(&sg) = rdma_len;		 // support "dynamically" chaging  R/W length !!!!!

	ret = ib_map_mr_sg(_cb->reg_mr_passive, &sg, 1, PAGE_SIZE);  // snyc ib_dma_sync_single_for_cpu/dev
	BUG_ON(ret <= 0 || ret > _cb->page_list_len);

	MSG_RDMA_PRK("%s(): ### post_inv = %d, reg_mr_wr_passive new rkey %d "
				 "pgsz %u len %u rdma_len (dynamical) %d iova_start %llx\n",
				 __func__, post_inv, _cb->reg_mr_wr_passive.key,
				 _cb->reg_mr_passive->page_size, _cb->reg_mr_passive->length,
										rdma_len, _cb->reg_mr_passive->iova);

	mutex_lock(&_cb->qp_mutex);
	if (post_inv)
		ret = ib_post_send(_cb->qp, &_cb->invalidate_wr_passive, &bad_wr); //
	else
		ret = ib_post_send(_cb->qp, &_cb->reg_mr_wr_passive.wr, &bad_wr); // called by only passive WRITE
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
 * User doen't have to take care of concurrency problem.
 * This func will take care of it.
 * User has to free the allocated mem  manually since they can reuse their buf
 */
int ib_kmsg_send_long(unsigned int dest_cpu,
					  struct pcn_kmsg_long_message *lmsg,
					  unsigned int payload_size)
{
	int ret;
	struct ib_send_wr *bad_wr;

	lmsg->hdr.size = payload_size + sizeof(lmsg->hdr); // total size
	DEBUG_LOG("%s() - payload_size %d sizeof(lmsg->hdr) %ld ack %s\n",
							__func__, payload_size, sizeof(lmsg->hdr),
									lmsg->hdr.rdma_ack?"true":"false");

	// check size with ib_send window size
	if( lmsg->hdr.size > sizeof(struct pcn_kmsg_long_message)) {
		printk("%s(): ERROR - MSG %d larger than MAX_MSG_SIZE %ld\n",
				__func__, lmsg->hdr.size, sizeof(struct pcn_kmsg_long_message));
		BUG_ON(-1);
	}

	lmsg->hdr.from_nid = my_cpu;

	if(dest_cpu==my_cpu) {
		// TODO: directly call the corresponding function pointer
		DEBUG_LOG("Jackmsglayer: itself %d\n", dest_cpu);
		DEBUG_LOG("Jackmsglayer: itself %d\n", dest_cpu);
		DEBUG_LOG("Jackmsglayer: itself %d\n", dest_cpu);
		DEBUG_LOG("Jackmsglayer: itself %d\n", dest_cpu);
		DEBUG_LOG("Jackmsglayer: itself %d\n", dest_cpu);
		return 0;
	}


	// pcn_msg (abstraction msg layer)
	//----------------------------------------------------------
	// ib

	/* rdma w/r */
	/*
	lmsg->hdr.remote_rkey = 0;  //normal send //TODO; the probelm for w/r sync
	lmsg->hdr.remote_addr = 0;  // normal send
	lmsg->hdr.rdma_size = 0;   // normal send
	*/

	MSG_SYNC_PRK("//////////////////lock() conn %d///////////////\n", dest_cpu);
	mutex_lock(&cb[dest_cpu]->send_mutex);

#if MSGDEBUG
	lmsg->hdr.ticket = atomic_inc_return(&g_send_ticket); // from 1
	DEBUG_LOG("%s() send ticket %d\n", __func__, lmsg->hdr.ticket);
#endif

	// copy form kernel buf to ib buf (passive_buf = send_buf!)
	if(unlikely(! memcpy(&cb[dest_cpu]->send_buf, lmsg, lmsg->hdr.size))) {
		BUG_ON(-1);
	}

	mutex_lock(&cb[dest_cpu]->qp_mutex);
	ret = ib_post_send(cb[dest_cpu]->qp, &cb[dest_cpu]->sq_wr, &bad_wr); // sq_wr is hardcoded used for send&recv, rdma_sq_wr for W/R
	mutex_unlock(&cb[dest_cpu]->qp_mutex);

	//wait_event(cb[dest_cpu]->sem, // since this // I don't have to have enq() deq()
	wait_event_interruptible(cb[dest_cpu]->sem, // since this // I don't have to have enq() deq()
				atomic_read(&(cb[dest_cpu]->state)) == RDMA_SEND_COMPLETE); // include all error

	atomic_set(&cb[dest_cpu]->state, IDLE); // dont let it stay err state // let others proceed
	mutex_unlock(&cb[dest_cpu]->send_mutex);
	MSG_SYNC_PRK("////////////////unlock() conn %d///////////////\n", dest_cpu);
	DEBUG_LOG_V("Jackmsglayer: 1 msg sent to dest_cpu %d!!!!!!\n\n", dest_cpu);
	return 0;
}


// if (payload_size >= PAGE_SIZE)
//	  R/W
// lmsg->hdr.type == PCN_KMSG_TYPE_RDMA_READ_REQUEST
// lmsg->hdr.type == PCN_KMSG_TYPE_RDMA_WRITE_REQUEST
/*
 *  user should free lmsg!
 */
int ib_kmsg_send_rdma(unsigned int dest_cpu, struct pcn_kmsg_rdma_message *lmsg,
					  unsigned int payload_size)
{
	uint32_t rkey;
	DEBUG_LOG_V("%s(): \n", __func__);

	if (payload_size > sizeof(*lmsg)) {
		if( unlikely(payload_size > MAX_RDMA_SIZE) ) {
			printk(KERN_ERR "%s(): ERROR - R/W size %u "
							"is larger than MAX_RDMA_SIZE %d\n",
							__func__, payload_size, MAX_RDMA_SIZE); // composing a W/R ACK (active)
		   BUG();
		}
		// return ib_kmsg_send_rdma(); //TODO
	}
	else {
		/* init rw only info then send */
		// lmsg->hdr.rw_ticket = 0;
		// ib_kmsg_send_rdma(); //TODO
	}

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

	// FIFO: make order for dbg
	//spin_lock(&cb[dest_cpu]->rw_slock);	 // TODO: test it try to remove it
	lmsg->hdr.rw_ticket = atomic_inc_return(&cb[dest_cpu]->g_all_ticket); // dbg
	//spin_unlock(&cb[dest_cpu]->rw_slock);   // TODO: test it try to remove it

	mutex_lock(&cb[dest_cpu]->active_mutex);
	atomic_inc(&cb[dest_cpu]->active_cnt);  // lock dbg

	if(lmsg->hdr.type == PCN_KMSG_TYPE_RDMA_READ_REQUEST) {
		MSG_SYNC_PRK("///////READ active   lock() %d rw_t %d conn %d ///////\n",
									(int)atomic_read(&cb[dest_cpu]->active_cnt),
									lmsg->hdr.rw_ticket, cb[dest_cpu]->conn_no);
	}
	else if(lmsg->hdr.type == PCN_KMSG_TYPE_RDMA_WRITE_REQUEST) {
		MSG_SYNC_PRK("////////WRITE active lock() %d  rw_t %d conn %d //////\n",
									(int)atomic_read(&cb[dest_cpu]->active_cnt),
									lmsg->hdr.rw_ticket, cb[dest_cpu]->conn_no);
	}
	else
		BUG_ON(-1);

	/* form rdma meta data */
	// rdma_dma_addr(rdma_buf) for active, start for passive
	DEBUG_LOG("krping_format_send(): \n"); // composing a W/R ACK (active)
	rkey = krping_rdma_rkey(cb[dest_cpu], cb[dest_cpu]->rdma_dma_addr,	  // rdma_dma_addr is sent for remote READ/WRITE
				!cb[dest_cpu]->server_invalidate, lmsg->hdr.rdma_size);	 // Jack failed to trun inv off
	lmsg->hdr.remote_addr = cb[dest_cpu]->rdma_dma_addr;					// TODO: test
	//lmsg->hdr.remote_addr = htonll(cb[dest_cpu]->rdma_dma_addr);			// rdma request - give locak v rdma addr to passive side. It was done in krping
	DEBUG_LOG("%s() - @@@ cb[%d] rkey %d cb[]->rdma_dma_addr %p "
										"lmsg->hdr.rdma_size %d\n",
										__func__, dest_cpu, rkey,
				(void*)cb[dest_cpu]->rdma_dma_addr, lmsg->hdr.rdma_size);
	lmsg->hdr.remote_rkey = htonl(rkey);									// rdma request

	lmsg->hdr.from_nid = my_cpu;
	//lmsg->hdr.size = 0; // total size. It's just a signal, size=0
	lmsg->hdr.rdma_ack = false;

	if(dest_cpu == my_cpu) {
		DEBUG_LOG("Jackmsglayer: itself %d\n", dest_cpu);
		DEBUG_LOG("Jackmsglayer: itself %d\n", dest_cpu);
		DEBUG_LOG("Jackmsglayer: itself %d\n", dest_cpu);
		DEBUG_LOG("Jackmsglayer: itself %d\n", dest_cpu);
		DEBUG_LOG("Jackmsglayer: itself %d\n", dest_cpu);
		// lookup_ftn(); // TODO 1: // TODO 2: ACK will be performed?
		return 0;
	}

	// pcn_msg (abstraction msg layer)
	//----------------------------------------------------------
	// ib
#if MSGDEBUG
	lmsg->hdr.rdma_ticket = atomic_inc_return(&g_rw_ticket); // from 1
	DEBUG_LOG("%s() rw ticket %d\n", __func__, lmsg->hdr.rdma_ticket);
#endif
	// send signal
	//memcpy(cb[dest_cpu]->rdma_buf, lmsg->hdr.your_buf_ptr, lmsg->hdr.rdma_size); // TODO: enable: fail unenable:testing
	pcn_kmsg_send_long(dest_cpu, (struct pcn_kmsg_long_message*) lmsg, 0); // hdr.size = 0

	DEBUG_LOG("Jackmsglayer: 1 rdma req snet through dest_cpu %d\n", dest_cpu);
	return 0;
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
