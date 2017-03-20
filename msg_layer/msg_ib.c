/*
 * pcn_kmesg.c - Kernel Module for Popcorn Messaging Layer over Socket
 * Author: Jack Chuang
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

#include <popcorn/pcn_kmsg.h>

#include <linux/fdtable.h>

#include <linux/net.h>
#include <net/sock.h>
#include <linux/tcp.h>
#include <linux/in.h>
#include <asm/uaccess.h>
#include <linux/socket.h>

#include <linux/delay.h>
#include <linux/time.h>
#include <asm/atomic.h>
#include <linux/completion.h>

//#include <popcorn/init.h> //Jack: deal with it later
#include <linux/cpumask.h>
#include <linux/sched.h>

#include <linux/vmalloc.h>
//#include "genif.h"

/* geting host ip */
#include <linux/netdevice.h>
#include <linux/inetdevice.h>
#include <linux/inet.h>

/* pci */
#include <linux/pci.h>
#include <asm/pci.h>


/* rdma */
#include <rdma/ib_verbs.h>
#include <rdma/rdma_cm.h>

/** Jack
 *  mssg layer multi-version
 *  msg sent to data_sock[conn_no] according to dest_cpu
 *
 *
 **/
/* global */
int my_cpu = -99; // source


/* Machines info !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!! */
#define MAX_NUM_NODES       2  // total num of machines
#define MAX_NUM_CHANNELS    MAX_NUM_NODES-1 //=MAX

/*
const char net_dev_name[]="eth0";  // testing
const uint32_t ip_table[MAX_NUM_NODES] ={ (10<<24 | 1<<16 | 1<<8 | 203),      // echo3 eth0
                                    (10<<24 | 1<<16 | 1<<8 | 204),      // echo4 eth0
                                    (10<<24 | 1<<16 | 1<<8 | 205) };    // none eth0
*/

char net_dev_name[]="ib0";
char* ip_table[MAX_NUM_NODES] ={ "192.168.69.127",       // echo3 ib0
                                "192.168.69.128",      // echo4 ib0
                                "192.168.69.129" };    // none ib0
// temporary solution................
uint32_t ip_table2[MAX_NUM_NODES] ={ (192<<24 | 168<<16 | 69<<8 | 127),      // echo3 ib0
                                    (192<<24 | 168<<16 | 69<<8 | 128),      // echo4 ib0
                                    (192<<24 | 168<<16 | 69<<8 | 254) };    // none ib0

// another special case, Xgene(ARM)
const char net_dev_name2[]="p7p1";

#define PORT 1000

//////////////////////////////////////rdma
//global
#define debug 1
#define KRPING_EXP_LOG 1
#define KRPING_EXP_DATA 1
#define force_debug 1

// for making sure data is good (not gaurantee it doesn't affect data)
#define EXP_LOG if(KRPING_EXP_LOG) printk
#define EXP_DATA if(KRPING_EXP_DATA) printk
#define DEBUG_LOG if(debug && !KRPING_EXP_DATA) printk
#define DEBUG_LOG if(force_debug) printk

#define KRPRINT_INIT printk

#define PFX ""
//#define PFX "krping: "

enum test_state {
    IDLE = 1,
    CONNECT_REQUEST,
    ADDR_RESOLVED,
    ROUTE_RESOLVED,
    CONNECTED,
    RDMA_READ_ADV,
    RDMA_READ_COMPLETE,
    RDMA_WRITE_ADV,
    RDMA_WRITE_COMPLETE,
    RDMA_SEND_COMPLETE,
    RDMA_RECV_COMPLETE,
    ERROR
};

struct krping_rdma_info {
    uint64_t buf;
    uint32_t rkey;
    //uint32_t size;
    uint64_t size;
};

struct krping_stats {
    unsigned long long send_bytes;
    unsigned long long send_msgs;
    unsigned long long recv_bytes;
    unsigned long long recv_msgs;
    unsigned long long write_bytes;
    unsigned long long write_msgs;
    unsigned long long read_bytes;
    unsigned long long read_msgs;
};


/*
 * Default max buffer size for IO...
 */
//#define RPING_BUFSIZE 128*1024
#define RPING_BUFSIZE 8*1024*1024 //Jack
#define RPING_SQ_DEPTH 64

/////////////////////Jack's testing & example code/////////////////////////////////////
typedef struct {
    struct pcn_kmsg_hdr hdr; // must followd
    // you define
    int example1;
    int example2;
    char msg[8192]; // testing lager than 1 MTU (cureent MAX is 12k)
}__attribute__((packed)) remote_thread_first_test_request_t; // for cache
/////////////////////Jack's testing & example code/////////////////////////////////////

/*
 * Control block struct.
 */
struct krping_cb {
    int server;         /* 0 iff client */
    struct ib_cq *cq;
    struct ib_pd *pd;
    struct ib_qp *qp;

    struct ib_mr *dma_mr;

    struct ib_fast_reg_page_list *page_list;
    int page_list_len;
    struct ib_reg_wr reg_mr_wr;
    struct ib_send_wr invalidate_wr;
    struct ib_mr *reg_mr;
    int server_invalidate;
    int read_inv;
    u8 key;

    struct ib_recv_wr rq_wr;    /* recv work request record */
    struct ib_sge recv_sgl;     /* recv single SGE */
    //struct krping_rdma_info recv_buf;/* malloc'd buffer */
    struct pcn_kmsg_long_message recv_buf; /* malloc'd buffer */ /* msg unit */

    u64 recv_dma_addr;
    //DECLARE_PCI_UNMAP_ADDR(recv_mapping) // cannot compile = =
    u64 recv_mapping;

    struct ib_send_wr sq_wr;    /* send work requrest record */
    struct ib_sge send_sgl;
    //struct krping_rdma_info send_buf;/* single send buf */  // JACK Iguess this is 24 bytes
    struct pcn_kmsg_long_message send_buf;/* single send buf */ /* msg unit */
    u64 send_dma_addr;
    //DECLARE_PCI_UNMAP_ADDR(send_mapping) // cannot compile = =
    u64 send_mapping;


    struct ib_rdma_wr rdma_sq_wr;   /* rdma work request record */
    struct ib_sge rdma_sgl;     /* rdma single SGE */
    char *rdma_buf;         /* used as rdma sink */
    u64  rdma_dma_addr;
    //DECLARE_PCI_UNMAP_ADDR(rdma_mapping) // cannot compile = =
    u64 rdma_mapping;
    struct ib_mr *rdma_mr;

    uint32_t remote_rkey;       /* remote guys RKEY */
    uint64_t remote_addr;       /* remote guys TO */
    uint32_t remote_len;        /* remote guys LEN */

    char *start_buf;        /* rdma read src */
    u64  start_dma_addr;
    //DECLARE_PCI_UNMAP_ADDR(start_mapping) // cannot compile = =
    u64 start_mapping;
    struct ib_mr *start_mr;

    enum test_state state;      /* used for cond/signalling */
    wait_queue_head_t sem;
    struct krping_stats stats;

    uint16_t port;          /* dst port in NBO */
    u8 addr[16];            /* dst addr in NBO */
    char *addr_str;         /* dst addr string */
    uint8_t addr_type;      /* ADDR_FAMILY - IPv4/V6 */
    int verbose;            /* verbose logging */
    int count;          /* ping count */
    //int size;         /* ping data size */
    unsigned long size;         /* ping data size */
    int validate;           /* validate ping data */
    int wlat;           /* run wlat test */
    int rlat;           /* run rlat test */
    int bw;             /* run bw test */
    int duplex;         /* run bw full duplex test */
    int poll;           /* poll or block for rlat test */
    int txdepth;            /* SQ depth */
    int local_dma_lkey;     /* use 0 for lkey */
    int frtest;         /* reg test */

    /* CM stuff */
    struct rdma_cm_id *cm_id;       /* connection on client side,*/
                                    /* listener on server side. */
    struct rdma_cm_id *child_cm_id; /* connection on server side */
    struct list_head list;
    int conn_no;
    unsigned long from_size;
    struct mutex send_mutex;
    struct mutex recv_mutex;
};


/////////////////////Jack's testing & example code/////////////////////////////////////
static void handle_remote_thread_first_test_request(struct pcn_kmsg_long_message* inc_lmsg){
    remote_thread_first_test_request_t* request = (remote_thread_first_test_request_t*) inc_lmsg;

    MSGPRINTK("<<<<< Jack MSG_LAYER SELF-TESTING: my_cpu=%d from_cpu=%d example1=%d example2=%d msg_layer(good) >>>>>\n",
            my_cpu, request->hdr.from_cpu, request->example1, request->example2);
    /* extra examples */
    // sync
    //down_read(&mm_data->kernel_set_sem);

    // new work
    //INIT_WORK( (struct work_struct*)request_work,
    //                   process_count_request);
    //queue_work(exit_wq, (struct work_struct*) request_work);

    // exit()
    kfree(request);
    return;
    // if you wanna do pong, plz remember you have to have another
    // struct remote_thread_first_test_response_t
}
/////////////////////Jack's testing & example code/////////////////////////////////////



// rdma
/*
 * List of running krping threads.
 */
static LIST_HEAD(krping_cbs);
static DEFINE_MUTEX(krping_mutex);
struct krping_cb *cb[MAX_NUM_NODES];

#define htonll(x) cpu_to_be64((x))
#define ntohll(x) cpu_to_be64((x))


int ib_kmsg_send_long(unsigned int dest_cpu, struct pcn_kmsg_long_message *lmsg, unsigned int payload_size); // triggered by user, doing enq() and then sending signal

//** Popcorn utility **//
uint32_t get_host_ip(char *tmp_net_dev_name);

volatile static int is_connection_done[MAX_NUM_NODES]; //Jack: atomic is more safe / use smp_mb()

/* for debug */
// check POPCORN_MSG_LAYER_VERBOSE in <linux/pcn_kmsg.h>
#define MSG_TEST 	0

static int __init initialize(void);

extern pcn_kmsg_cbftn callbacks[PCN_KMSG_TYPE_MAX];
extern send_cbftn send_callback;


/* funcs */
static int krping_create_qp(struct krping_cb *cb);
static int client_recv(struct krping_cb *cb, struct ib_wc *wc);
static int server_recv(struct krping_cb *cb, struct ib_wc *wc);
static int ib_kmsg_recv_long(struct krping_cb *cb, struct ib_wc *wc);

/* workqueue */
struct workqueue_struct *msg_handler;

/* workqueue arg */
typedef struct {
    struct work_struct work;
    struct pcn_kmsg_long_message *lmsg;
    //enum pcn_kmsg_wq_ops op;
    //int from_cpu;
    //int cpu_to_add;
    //pcn_kmsg_mcast_id id_to_join;
} pcn_kmsg_work_t;

void *pcn_kmsg_alloc_msg_long(size_t size) {
    struct pcn_kmsg_long_message *msg = NULL;
    msg = (struct pcn_kmsg_long_message *) kmalloc(size, GFP_KERNEL);
    msg->hdr.size = size;
    return (void *) msg;
}

static int krping_cma_event_handler(struct rdma_cm_id *cma_id,
                   struct rdma_cm_event *event)
{
    int ret;
    struct krping_cb *cb = cma_id->context;
    DEBUG_LOG("external>>>>>>%s();\n", __func__);

    DEBUG_LOG("cma_event type %d cma_id %p (%s)\n", event->event, cma_id,
          (cma_id == cb->cm_id) ? "parent" : "child");

    switch (event->event) {
    case RDMA_CM_EVENT_ADDR_RESOLVED:
        cb->state = ADDR_RESOLVED;
        ret = rdma_resolve_route(cma_id, 2000);
        if (ret) {
            printk(KERN_ERR PFX "rdma_resolve_route error %d\n",
                   ret);
            wake_up_interruptible(&cb->sem);
        }
        break;

    case RDMA_CM_EVENT_ROUTE_RESOLVED:
        cb->state = ROUTE_RESOLVED;
        wake_up_interruptible(&cb->sem);
        break;

    case RDMA_CM_EVENT_CONNECT_REQUEST:
        cb->state = CONNECT_REQUEST;
        cb->child_cm_id = cma_id;
        DEBUG_LOG("child cma %p\n", cb->child_cm_id);
        wake_up_interruptible(&cb->sem);
        break;

    case RDMA_CM_EVENT_ESTABLISHED:
        DEBUG_LOG("CONNECTION ESTABLISHED\n");
        //if (!cb->server) { // JackM
            cb->state = CONNECTED;
        //}
        DEBUG_LOG("%s(): cb->state=%d, CONNECTED=%d\n", __func__, cb->state, CONNECTED);
        wake_up(&cb->sem); // TODO: testing: change back, see if it runs as well
        //wake_up_interruptible(&cb->sem); // default:
        break;

    case RDMA_CM_EVENT_ADDR_ERROR:
    case RDMA_CM_EVENT_ROUTE_ERROR:
    case RDMA_CM_EVENT_CONNECT_ERROR:
    case RDMA_CM_EVENT_UNREACHABLE:
    case RDMA_CM_EVENT_REJECTED:
        printk(KERN_ERR PFX "cma event %d, error %d\n", event->event,
               event->status);
        cb->state = ERROR;
        wake_up_interruptible(&cb->sem);
        break;

    case RDMA_CM_EVENT_DISCONNECTED:
        DEBUG_LOG("%s(): cb->state=%d, CONNECTED=%d\n", __func__, cb->state, CONNECTED);
        printk(KERN_ERR PFX "DISCONNECT EVENT...\n");
        cb->state = ERROR;
        wake_up_interruptible(&cb->sem);
        break;

    case RDMA_CM_EVENT_DEVICE_REMOVAL:
        printk(KERN_ERR PFX "cma detected device removal!!!!\n");
        break;

    default:
        printk(KERN_ERR PFX "oof bad type!\n");
        wake_up_interruptible(&cb->sem);
        break;
    }
    return 0;
}


static void krping_cq_event_handler(struct ib_cq *cq, void *ctx)
{
    struct krping_cb *cb = ctx;
    struct ib_wc wc;
    struct ib_recv_wr *bad_wr;
    int ret;

    DEBUG_LOG("\nexternal------>%s();\n", __func__);

    BUG_ON(cb->cq != cq);
    if (cb->state == ERROR) {
        printk(KERN_ERR PFX "cq completion in ERROR state\n");
        return;
    }
    //if (cb->frtest) {
    //    printk(KERN_ERR PFX "cq completion event in frtest!\n");
    //    return;
    //}
    //if (!cb->wlat && !cb->rlat && !cb->bw) {
        ib_req_notify_cq(cb->cq, IB_CQ_NEXT_COMP);
    //}
    //while ((ret = ib_poll_cq(cb->cq, 1, &wc)) == 1) { //fail
    while ((ret = ib_poll_cq(cb->cq, 1, &wc)) > 0) { //fail
        /**
         * IBV_WC_LOC_PROT_ERR (4) -
         * This event is generated when a user attempts to access an address outside of the registered memory
         * region. For example, this may happen if the Lkey does not match the address in the WR.
         *
         * IBV_WC_LOC_PROT_ERR (4) - Local Protection Error: the locally posted Work Request’s buffers in the
         * scatter/gather list does not reference a Memory Region that is valid for the requested operation.
         *
         *
         **/
        if (wc.status) { // !=IBV_WC_SUCCESS
            if (wc.status == IB_WC_WR_FLUSH_ERR) {
                DEBUG_LOG("cq flushed\n");
                continue;
            } else {
                printk(KERN_ERR PFX "cq completion failed with "
                       "wr_id %Lx status %d opcode %d vender_err %x\n",
                    wc.wr_id, wc.status, wc.opcode, wc.vendor_err);
                goto error;
            }
        }

        switch (wc.opcode) {
        case IB_WC_SEND:
            DEBUG_LOG("----- SEND COMPLETION -----\n");
            cb->stats.send_bytes += cb->send_sgl.length;
            cb->stats.send_msgs++;
            cb->state = RDMA_SEND_COMPLETE;
            mutex_unlock(&cb->send_mutex);
            wake_up_interruptible(&cb->sem); // Jack: added by Jack Should I add this? // TODO: take out and test
            break;

        case IB_WC_RDMA_WRITE:
            DEBUG_LOG("----- RDMA WRITE COMPLETION ----- (good)\n");
            cb->stats.write_bytes += cb->rdma_sq_wr.wr.sg_list->length;
            cb->stats.write_msgs++;
            cb->state = RDMA_WRITE_COMPLETE;
            wake_up_interruptible(&cb->sem);
            break;

        case IB_WC_RDMA_READ:
            DEBUG_LOG("----- RDMA READ COMPLETION ----- (good)\n");
            cb->stats.read_bytes += cb->rdma_sq_wr.wr.sg_list->length;
            cb->stats.read_msgs++;
            cb->state = RDMA_READ_COMPLETE;
            wake_up_interruptible(&cb->sem);
            break;


        case IB_WC_RECV:
            DEBUG_LOG("----- RECV COMPLETION -----\n");
            cb->stats.recv_bytes += sizeof(cb->recv_buf);
            cb->stats.recv_msgs++;
            cb->state = RDMA_RECV_COMPLETE; // skip this, directly process

            ret = ib_kmsg_recv_long(cb, &wc);
            if (ret) {
                printk(KERN_ERR PFX "recv wc error: %d\n", ret);
                goto error;
            }

            DEBUG_LOG("ib_post_recv (something completed) <check recv buf> \n");
            ret = ib_post_recv(cb->qp, &cb->rq_wr, &bad_wr);
            if (ret) {
                printk(KERN_ERR PFX "post recv error: %d\n",
                       ret);
                goto error;
            }
            wake_up_interruptible(&cb->sem);
            break;

        default:
            printk(KERN_ERR PFX
                   "%s:%d Unexpected opcode %d, Shutting down\n",
                   __func__, __LINE__, wc.opcode);
            goto error;
        }
    }
    if (ret) {
        printk(KERN_ERR PFX "poll error %d\n", ret);
        goto error;
    }
    return;
error:
    cb->state = ERROR;
    wake_up_interruptible(&cb->sem);
}

static int krping_connect_client(struct krping_cb *cb)
{
    struct rdma_conn_param conn_param;
    int ret;

    DEBUG_LOG("\n->%s();\n", __func__);

    memset(&conn_param, 0, sizeof conn_param);
    conn_param.responder_resources = 1;
    conn_param.initiator_depth = 1;
    conn_param.retry_count = 10;

    ret = rdma_connect(cb->cm_id, &conn_param);
    if (ret) {
        printk(KERN_ERR PFX "rdma_connect error %d\n", ret);
        return ret;
    }

    wait_event_interruptible(cb->sem, cb->state >= CONNECTED);
    if (cb->state == ERROR) {
        printk(KERN_ERR PFX "wait for CONNECTED state %d\n", cb->state);
        return -1;
    }

    DEBUG_LOG("rdma_connect successful\n");
    // Jack: accep(done);
    //is_connection_done[cb->conn_no] = 1;
    //smp_mb(); // Jack: since my_cpu is extern (global)
    return 0;
}

void *pcn_kmsg_alloc_msg(size_t size) {
    struct pcn_kmsg_message *msg = NULL;
    msg = (struct pcn_kmsg_message *) kmalloc(size, GFP_KERNEL);
    msg->hdr.size = size;
    return (void *) msg;
}

/*
 * Jack utility -
 */
uint32_t get_host_ip(char *tmp_net_dev_name) {
    // way1.
    /*
    struct net_device *dev = __dev_get_by_name("eth0");
    dev->dev_addr; // is the MAC address
    dev->stats.rx_dropped; // RX dropped packets. (stats has more statistics)

    struct in_device *in_dev = rcu_dereference(dev->ip_ptr);
    // in_dev has a list of IP addresses (because an interface can have multiple)
    struct in_ifaddr *ifap;
    for (ifap = in_dev->ifa_list; ifap != NULL;
        ifap = ifa1->ifa_next) {
        ifap->ifa_address; // is the IPv4 address
    }
    */
    // way2.
    struct net_device *device;
    struct in_device *in_dev;
    struct in_ifaddr *if_info;

    __u8 *addr;

    //sprintf(net_dev_name,"eth%d",ethernet_id);  // =strcat()
    device = __dev_get_by_name(&init_net, net_dev_name); // namespace=normale
    if(device) {
        tmp_net_dev_name = (char *) vmalloc(sizeof(net_dev_name));
        memcpy(tmp_net_dev_name, net_dev_name, sizeof(net_dev_name));
        in_dev = (struct in_device *)device->ip_ptr;
        if_info = in_dev->ifa_list;
        addr = (char *)&if_info->ifa_local;
        /*
         MSGDPRINTK(KERN_WARNING "Device %s IP: %u.%u.%u.%u\n",
                        net_dev_name,
                        (__u32)addr[0],
                        (__u32)addr[1],
                        (__u32)addr[2],
                        (__u32)addr[3]);
        */
        return (addr[0]<<24 | addr[1]<<16 | addr[2]<<8 | addr[3]);
    }
    else{
        //sprintf(net_dev_name2,"eth%d",ethernet_id);
        device = __dev_get_by_name(&init_net, net_dev_name2);
        if(device) {
            tmp_net_dev_name = (char *) vmalloc(sizeof(net_dev_name2));
            memcpy(tmp_net_dev_name, net_dev_name2, sizeof(net_dev_name2));
            in_dev = (struct in_device *)device->ip_ptr;
            if_info = in_dev->ifa_list;
            addr = (char *)&if_info->ifa_local;
            MSGDPRINTK(KERN_WARNING "Device2 %s IP: %u.%u.%u.%u\n",
                            net_dev_name2,
                            (__u32)addr[0],
                            (__u32)addr[1],
                            (__u32)addr[2],
                            (__u32)addr[3]);
            return (addr[0]<<24 | addr[1]<<16 | addr[2]<<8 | addr[3]);
        }
        else {
            MSGPRINTK(KERN_ERR "Jackmsglayer: ERROR - cannot find host ip (eth0/p7p1)\n");
            return -1;
        }
    }
}

static void fill_sockaddr(struct sockaddr_storage *sin, struct krping_cb *_cb)
{
    memset(sin, 0, sizeof(*sin));

    if(!_cb->server){ // TODO: whould it be a potential bug?
        if (_cb->addr_type == AF_INET) { // client: load as usuall (ip=remote)
            struct sockaddr_in *sin4 = (struct sockaddr_in *)sin;
            sin4->sin_family = AF_INET;
            memcpy((void *)&sin4->sin_addr.s_addr, _cb->addr, 4);
            sin4->sin_port = _cb->port;
        }
        printk("Jack client IP fillup _cb->addr %s _cb->port %d\n", _cb->addr, _cb->port);
    }
    else{ // cb->server: load from global (ip=itself)
        if (cb[my_cpu]->addr_type == AF_INET) {
            struct sockaddr_in *sin4 = (struct sockaddr_in *)sin;
            sin4->sin_family = AF_INET;
            memcpy((void *)&sin4->sin_addr.s_addr, cb[my_cpu]->addr, 4);
            sin4->sin_port = cb[my_cpu]->port;
            printk("Jack server IP fillup cb[my_cpu]->addr %s cb[my_cpu]->port %d\n", cb[my_cpu]->addr, cb[my_cpu]->port);
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

static int reg_supported(struct ib_device *dev)
{
    u64 needed_flags = IB_DEVICE_MEM_MGT_EXTENSIONS |
               IB_DEVICE_LOCAL_DMA_LKEY;
    struct ib_device_attr device_attr;
    int ret;
    ret = ib_query_device(dev, &device_attr);
    //DEBUG_LOG("%s(): return %d\n", __func__, ret);
    //DEBUG_LOG("%s(): needed_flags %llx\n", __func__, needed_flags);
    //DEBUG_LOG("%s(): device_attr.device_cap_flag %llx\n", __func__, device_attr.device_cap_flags);
    DEBUG_LOG("%s(): IB_DEVICE_MEM_MGT_EXTENSIONS %llx\n", __func__, IB_DEVICE_MEM_MGT_EXTENSIONS);
    DEBUG_LOG("%s(): IB_DEVICE_LOCAL_DMA_LKEY %llx\n", __func__, IB_DEVICE_LOCAL_DMA_LKEY);
    DEBUG_LOG("%s(): (device_attr.device_cap_flags & needed_flags) %llx\n", __func__, (device_attr.device_cap_flags & needed_flags));

    if ((device_attr.device_cap_flags & needed_flags) != needed_flags) {
        printk(KERN_ERR PFX
            "Fastreg not supported - device_cap_flags 0x%llx\n",
            (u64)device_attr.device_cap_flags);
        //return 0;
        return 1;
    }
    DEBUG_LOG("Fastreg/local_dma_lkey supported - device_cap_flags 0x%llx\n",
        (u64)device_attr.device_cap_flags);

    /*
    u64 needed_flags = IB_DEVICE_MEM_MGT_EXTENSIONS |
               IB_DEVICE_LOCAL_DMA_LKEY;

    if ((dev->attrs.device_cap_flags & needed_flags) != needed_flags) {
    //if ((dev->ib_device_attr.device_cap_flags & needed_flags) != needed_flags) {
        printk(KERN_ERR PFX
            "Fastreg not supported - device_cap_flags 0x%llx\n",
            (u64)dev->attrs.device_cap_flags);
            //(u64)dev->ib_device_attr.device_cap_flags);
        return 0;
    }
    //DEBUG_LOG("Fastreg/local_dma_lkey supported - device_cap_flags 0x%llx\n",
    DEBUG_LOG("Fastreg/local_dma_lkey supported - device_cap_flags 0x%llx\n",
        (u64)dev->attrs.device_cap_flags);
        //(u64)dev->ib_device_attr.device_cap_flags);
    */
    //DEBUG_LOG("Fastreg/local_dma_lkey supported - device_cap_flags ??? (Jack: skip this check)\n");
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
        printk(KERN_ERR PFX "rdma_bind_addr error %d\n", ret);
        return ret;
    }

    DEBUG_LOG("rdma_listen\n");
    ret = rdma_listen(cb->cm_id, 99); // Jack: TODO: don't hardcode
    if (ret) {
        printk(KERN_ERR PFX "rdma_listen failed: %d\n", ret);
        return ret;
    }

    wait_event_interruptible(cb->sem, cb->state >= CONNECT_REQUEST); // krping_cma_event_handler
    if (cb->state != CONNECT_REQUEST) {
        printk(KERN_ERR PFX "wait for CONNECT_REQUEST state %d\n",
            cb->state);
        return -1;
    }

    if (!reg_supported(cb->child_cm_id->device)) //Jack
        return -EINVAL;

    return 0;
}

static void krping_setup_wr(struct krping_cb *cb) // set up sgl, used for rdma
{
    cb->recv_sgl.addr = cb->recv_dma_addr; // addr
    //cb->recv_sgl.addr = cb->recv_buf.buf; // wrong: canno use kernel addr
    cb->recv_sgl.length = sizeof cb->recv_buf;  //sizeof cb->recv_buf(16)
                                                //sizeof cb->recv_buf.buf(8)
    DEBUG_LOG("sizeof cb->recv_buf=%d\n", sizeof cb->recv_buf);
    //cb->recv_sgl.lkey = cb->qp->device->local_dma_lkey; // Jack: FOUND A SERVERE BUG!!! WRONG (0)
    cb->rq_wr.sg_list = &cb->recv_sgl;
    cb->rq_wr.num_sge = 1;

    cb->send_sgl.addr = cb->send_dma_addr; // addr
    //cb->send_sgl.addr = cb->send_buf.buf; // wrong: cannot use kernel addr
    cb->send_sgl.length = sizeof cb->send_buf;
    //cb->send_sgl.lkey = cb->qp->device->local_dma_lkey; // Jack: FOUND A SERVERE BUG!!! WRONG (0)

    //3
    cb->recv_sgl.lkey = cb->pd->local_dma_lkey; // correct
    cb->send_sgl.lkey = cb->pd->local_dma_lkey; // correct
    //4
    //cb->recv_sgl.lkey = cb->reg_mr->lkey; // never tried. TODO: try
    //cb->send_sgl.lkey = cb->reg_mr->lkey; // never tried. TODO: try

    DEBUG_LOG("@@@ <addr>\n");
    DEBUG_LOG("@@@ 2 cb->recv_sgl.addr = %p\n", cb->recv_sgl.addr); // this is not local_recv_buffer // it's exhanged local addr to remote
    DEBUG_LOG("@@@ 2 cb->recv_dma_addr = %p\n", cb->recv_dma_addr); // v      addr (O) (mapped to each other)
    DEBUG_LOG("@@@ 2 sizeof cb->send_buf = %p\n", sizeof cb->send_buf);   // kernel addr (X) (mapped to each other)
    DEBUG_LOG("@@@ 2 cb->send_sgl.addr = %p\n", cb->send_sgl.addr);
    DEBUG_LOG("@@@ 2 cb->send_dma_addr = %p\n", cb->send_dma_addr); // v      addr

    DEBUG_LOG("@@@ <lkey>\n");
    DEBUG_LOG("@@@ 2 cb->qp->device->local_dma_lkey = %d\n", cb->qp->device->local_dma_lkey);    //0
    DEBUG_LOG("@@@ 3lkey=%d from ../mad.c (ctx->pd->local_dma_lkey)\n", cb->pd->local_dma_lkey); //4450 dynamic
    DEBUG_LOG("@@@ 4lkey=%d from client\/server example(cb->mr->lkey)\n", cb->reg_mr->lkey);     //4463 dynamic

    cb->sq_wr.opcode = IB_WR_SEND; // normal send / recv
    cb->sq_wr.send_flags = IB_SEND_SIGNALED;
    cb->sq_wr.sg_list = &cb->send_sgl; // sge
    cb->sq_wr.num_sge = 1;

    //if (cb->server || cb->wlat || cb->rlat || cb->bw) { //(only server setup rdma_sq_wr since only server does RDMA)
        DEBUG_LOG("only server setup rdma_sq_wr\n");
        cb->rdma_sgl.addr = cb->rdma_dma_addr;

        cb->rdma_sq_wr.wr.sg_list = &cb->rdma_sgl;
        cb->rdma_sq_wr.wr.send_flags = IB_SEND_SIGNALED;
        cb->rdma_sq_wr.wr.num_sge = 1;
    //}

    /*
     * A chain of 2 WRs, INVALDATE_MR + REG_MR.
     * both unsignaled.  The client uses them to reregister
     * the rdma buffers with a new key each iteration.
     */
    // Jack: check can we fix the key
    cb->reg_mr_wr.wr.opcode = IB_WR_REG_MR;     //(legacy:fastreg)
    cb->reg_mr_wr.mr = cb->reg_mr;

    cb->invalidate_wr.next = &cb->reg_mr_wr.wr;
    cb->invalidate_wr.opcode = IB_WR_LOCAL_INV;
}

static int krping_setup_qp(struct krping_cb *cb, struct rdma_cm_id *cm_id)
{
    int ret;
    struct ib_cq_init_attr attr = {0};

    DEBUG_LOG("\n->%s();\n", __func__);

    //cb->pd = ib_alloc_pd(cm_id->device, 0);
    cb->pd = ib_alloc_pd(cm_id->device);
    if (IS_ERR(cb->pd)) {
        printk(KERN_ERR PFX "ib_alloc_pd failed\n");
        return PTR_ERR(cb->pd);
    }
    DEBUG_LOG("created pd %p\n", cb->pd);

    attr.cqe = cb->txdepth * 2;
    attr.comp_vector = 0;
    cb->cq = ib_create_cq(cm_id->device, krping_cq_event_handler, NULL,
                  cb, &attr);
    if (IS_ERR(cb->cq)) {
        printk(KERN_ERR PFX "ib_create_cq failed\n");
        ret = PTR_ERR(cb->cq);
        goto err1;
    }
    DEBUG_LOG("created cq %p task\n", cb->cq);

    //if (!cb->wlat && !cb->rlat && !cb->bw && !cb->frtest) {
        ret = ib_req_notify_cq(cb->cq, IB_CQ_NEXT_COMP);
        if (ret) {
            printk(KERN_ERR PFX "ib_create_cq failed\n");
            goto err2;
        }
    //}

    ret = krping_create_qp(cb);
    if (ret) {
        printk(KERN_ERR PFX "krping_create_qp failed: %d\n", ret);
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

static int krping_setup_buffers(struct krping_cb *cb) // init all buffers < 1.pd->cq->qp 2.[mr] 3.xxx >
{
    int ret;
    DEBUG_LOG("\n->%s();\n", __func__);

    DEBUG_LOG(PFX "krping_setup_buffers called on cb %p\n", cb);

    cb->recv_dma_addr = dma_map_single(cb->pd->device->dma_device,  // for remote access  (mapping together)
                   &cb->recv_buf,                                   // for local access (mapping together)
                   sizeof(cb->recv_buf), DMA_BIDIRECTIONAL);        // cb->recv_dma
    pci_unmap_addr_set(cb, recv_mapping, cb->recv_dma_addr);
    cb->send_dma_addr = dma_map_single(cb->pd->device->dma_device,  // send
                       &cb->send_buf, sizeof(cb->send_buf),         // use send buffer
                       DMA_BIDIRECTIONAL);                          // cb->send_dma
    pci_unmap_addr_set(cb, send_mapping, cb->send_dma_addr);

    cb->rdma_buf = kmalloc(cb->size, GFP_KERNEL);                   // alloc rdma buffer (the only allocated buf)
    if (!cb->rdma_buf) {
        DEBUG_LOG(PFX "rdma_buf malloc failed\n");
        ret = -ENOMEM;
        goto bail;
    }
    cb->rdma_dma_addr = dma_map_single(cb->pd->device->dma_device,  //
                   cb->rdma_buf, cb->size,                          // userdma buffer
                   DMA_BIDIRECTIONAL);                              //
    pci_unmap_addr_set(cb, rdma_mapping, cb->rdma_dma_addr);

    DEBUG_LOG("@@@ 1 cb->rdma_buf = dma_map_single( cb->rdma_buf )\n");
    DEBUG_LOG("@@@ 1 cb->rdma_buf = 0x%p \t a ????? for local access (mapping together)\n", cb->rdma_buf);
    DEBUG_LOG("@@@ 1 cb->rdma_dma_addr = 0x%p a kernel vaddr for remote access  (mapping together)\n", cb->rdma_dma_addr);

    cb->page_list_len = (((cb->size - 1) & PAGE_MASK) + PAGE_SIZE)
                >> PAGE_SHIFT;
    cb->reg_mr = ib_alloc_mr(cb->pd,  IB_MR_TYPE_MEM_REG, // fill up lkey and rkey
                 cb->page_list_len);
    if (IS_ERR(cb->reg_mr)) {
        ret = PTR_ERR(cb->reg_mr);
        DEBUG_LOG(PFX "recv_buf reg_mr failed %d\n", ret);
        goto bail;
    }

    DEBUG_LOG("@@@ reg rkey %d page_list_len %u\n",
                    cb->reg_mr->rkey, cb->page_list_len);

    DEBUG_LOG("@@@ 1 Jack cb->reg_mr->lkey %d from mr \n", cb->reg_mr->lkey);
    DEBUG_LOG("@@@ 1 Jack cb->send_sgl.lkey %d from mr \n", cb->send_sgl.lkey);
    DEBUG_LOG("@@@ 1 Jack cb->recv_sgl.lkey %d from mr \n", cb->recv_sgl.lkey);
    DEBUG_LOG("@@@ 1 correct lkey=%d (ref: ./drivers/infiniband/core/mad.c ) \
            (ctx->pd->local_dma_lkey)\n", cb->pd->local_dma_lkey);     //4450 dynamic

    /*
        // these are all NULL.
        cb->dma_mr->lkey, cb->dma_mr->rkey
        cb->rdma_mr->lkey, cb->rdma_mr->rkey
        cb->start_mr->lkey, cb->start_mr->rkey
    */
    //if (!cb->server || cb->wlat || cb->rlat || cb->bw) { // only client generates rdma address for server
        DEBUG_LOG("only client(not true) setup start_buf start_dma_addr\n");
        cb->start_buf = kmalloc(cb->size, GFP_KERNEL);
        if (!cb->start_buf) {
            DEBUG_LOG(PFX "start_buf malloc failed\n");
            ret = -ENOMEM;
            goto bail;
        }

        cb->start_dma_addr = dma_map_single(cb->pd->device->dma_device,
                           cb->start_buf, cb->size, // only client generates rdma address for server
                           DMA_BIDIRECTIONAL);
        DEBUG_LOG("@@@ cb->start_dma_addr = 0x%lx Jack (only client->not true)\n", cb->start_dma_addr);
        pci_unmap_addr_set(cb, start_mapping, cb->start_dma_addr);
        DEBUG_LOG("@@@ cb->start_dma_addr = 0x%lx Jack (only client->not true)\n", cb->start_dma_addr);
    //}

    krping_setup_wr(cb); //(only server setup rdma_sq_wr since only server issue rdma operations)
    DEBUG_LOG(PFX "allocated & registered buffers...\n");
    DEBUG_LOG("\n\n");
    return 0;
bail:
    if (cb->reg_mr && !IS_ERR(cb->reg_mr))
        ib_dereg_mr(cb->reg_mr);
    if (cb->rdma_mr && !IS_ERR(cb->rdma_mr))
        ib_dereg_mr(cb->rdma_mr);
    if (cb->dma_mr && !IS_ERR(cb->dma_mr))
        ib_dereg_mr(cb->dma_mr);
    if (cb->rdma_buf)
        kfree(cb->rdma_buf);
    if (cb->start_buf)
        kfree(cb->start_buf);
    return ret;
}


static int krping_accept(struct krping_cb *cb)
{
    struct rdma_conn_param conn_param;
    int ret;
    DEBUG_LOG("\n->%s();\n", __func__);
    DEBUG_LOG("\taccepting client connection request......\n");

    memset(&conn_param, 0, sizeof conn_param);
    conn_param.responder_resources = 1;
    conn_param.initiator_depth = 1;

    ret = rdma_accept(cb->child_cm_id, &conn_param);
    if (ret) {
        printk(KERN_ERR PFX "rdma_accept error: %d\n", ret);
        return ret;
    }

    if (!cb->wlat && !cb->rlat && !cb->bw) {
        DEBUG_LOG("%s(): wating for a signal......\n", __func__);
        //wait_event_interruptible(cb->sem, cb->state >= CONNECTED);
        wait_event(cb->sem, cb->state >= CONNECTED);
        DEBUG_LOG("%s(): got the signal !!!!(GOOD)!!!!!!! cb->state=%d \n", __func__, cb->state);
        if (cb->state == ERROR) {
            printk(KERN_ERR PFX "wait for CONNECTED state %d\n",
                cb->state);
            return -1;
        }
    }
    // Jack: accep(done);
    //is_connection_done[cb->conn_no] = 1;
    //smp_mb(); // Jack: since my_cpu is extern (global)
    DEBUG_LOG("\n");
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

    dma_unmap_single(cb->pd->device->dma_device,
             pci_unmap_addr(cb, recv_mapping),
             sizeof(cb->recv_buf), DMA_BIDIRECTIONAL);
    dma_unmap_single(cb->pd->device->dma_device,
             pci_unmap_addr(cb, send_mapping),
             sizeof(cb->send_buf), DMA_BIDIRECTIONAL);
    dma_unmap_single(cb->pd->device->dma_device,
             pci_unmap_addr(cb, rdma_mapping),
             cb->size, DMA_BIDIRECTIONAL);
    kfree(cb->rdma_buf);
    if (cb->start_buf) {
        dma_unmap_single(cb->pd->device->dma_device,
             pci_unmap_addr(cb, start_mapping),
             cb->size, DMA_BIDIRECTIONAL);
        kfree(cb->start_buf);
    }
}

static void krping_free_qp(struct krping_cb *cb)
{
    ib_destroy_qp(cb->qp);
    ib_destroy_cq(cb->cq);
    ib_dealloc_pd(cb->pd);
}


static int krping_run_server(struct krping_cb *cb)
{
    struct ib_recv_wr *bad_wr;
    int ret;

    DEBUG_LOG("@@@ %s(): cb->conno %d\n", __func__, cb->conn_no);

    ret = krping_bind_server(cb);
    if (ret)
        return ret;
    DEBUG_LOG("\n\n\n");


    ret = krping_setup_qp(cb, cb->child_cm_id);
    if (ret) {
        printk(KERN_ERR PFX "setup_qp failed: %d\n", ret);
        goto err0;
    }

    ret = krping_setup_buffers(cb);
    if (ret) {
        printk(KERN_ERR PFX "krping_setup_buffers failed: %d\n", ret);
        goto err1;
    }
    // so far you can do send/recv

    DEBUG_LOG("ib_post_recv(manually)<<<<\n");
    ret = ib_post_recv(cb->qp, &cb->rq_wr, &bad_wr);
    if (ret) {
        printk(KERN_ERR PFX "ib_post_recv failed: %d\n", ret);
        goto err2;
    }

    ret = krping_accept(cb);
    if (ret) {
        printk(KERN_ERR PFX "connect error %d\n", ret);
        goto err2;
    }

    /*
    if (cb->wlat)
        printk("not supported\n");
        //krping_wlat_test_server(cb);
    else if (cb->rlat)
        printk("not supported\n");
        //krping_rlat_test_server(cb);
    else if (cb->bw)
        printk("not supported\n");
        //krping_bw_test_server(cb);
    else
        krping_test_server(cb);
    */

    //is_connection_done[cb->conn_no] = 1; //Jack: atomic is more safe
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

static int krping_bind_client(struct krping_cb *cb)
{
    struct sockaddr_storage sin;
    int ret;

    fill_sockaddr(&sin, cb);

    ret = rdma_resolve_addr(cb->cm_id, NULL, (struct sockaddr *)&sin, 2000);
    if (ret) {
        printk(KERN_ERR PFX "rdma_resolve_addr error %d\n", ret);
        return ret;
    }

    wait_event_interruptible(cb->sem, cb->state >= ROUTE_RESOLVED);
    if (cb->state != ROUTE_RESOLVED) {
        printk(KERN_ERR PFX
               "addr/route resolution did not resolve: state %d\n",
               cb->state);
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
    init_attr.cap.max_recv_wr = 2;

    /* For flush_qp() */
    init_attr.cap.max_send_wr++;
    init_attr.cap.max_recv_wr++;

    init_attr.cap.max_recv_sge = 1;
    init_attr.cap.max_send_sge = 1;
    init_attr.qp_type = IB_QPT_RC;
    init_attr.send_cq = cb->cq;
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

/* action for bottom half */
static void pcn_kmsg_handler_BottomHalf(struct work_struct * work)
{
    pcn_kmsg_work_t *w = (pcn_kmsg_work_t *) work;
    struct pcn_kmsg_long_message *lmsg =  w->lmsg;
    pcn_kmsg_cbftn ftn; // function pointer - typedef int (*pcn_kmsg_cbftn)(struct pcn_kmsg_message *);

    if( lmsg->hdr.type < 0 || lmsg->hdr.type >= PCN_KMSG_TYPE_MAX){
        MSGDPRINTK(KERN_INFO "Received invalid message type %d > MAX %d\n", lmsg->hdr.type, PCN_KMSG_TYPE_MAX);
        pcn_kmsg_free_msg(lmsg);
    }else{
       ftn = callbacks[lmsg->hdr.type];
        if(ftn != NULL){
            ftn(lmsg); // Jack: invoke callback with input arguments // callback has to kfree the buffer
        }else{
            MSGDPRINTK(KERN_INFO "Recieved message type %d size %d has no registered callback!\n", lmsg->hdr.type, lmsg->hdr.size);
            pcn_kmsg_free_msg(lmsg);
        }
    }
}

/*
 * parse recved msg in the buf to msg_layer
 */
static int ib_kmsg_recv_long(struct krping_cb *cb, struct ib_wc *wc)
{
    printk("%s():\n", __func__);
    // check msg healthy
    if (wc->byte_len != sizeof(cb->recv_buf)) {
        printk(KERN_ERR PFX "Received bogus data, size %d\n",
               wc->byte_len);
        return -1;
    }

    // ib (data in cb->recv_buf)
    //----------------------------------------------------------
    // pcn_msg (abstraction msg layer)

    // got msg form ib, attach to msg_layer.
    // !! don't transfer to user defined type. that should be done in user's handler

    /*
        // example - directly process
        remote_thread_first_test_request_t* request = (remote_thread_first_test_request_t*) &cb->recv_buf; // int buf
        printk("%s(): request->hdr.from_cpu %d\n", __func__, request->hdr.from_cpu);
        printk("urlmsg(u def)->example1 %d IF SEE 1 PREPARE GO HOME\n", request->example1);
        printk("urlmsg(u def)->example2 %d IF SEE 2 PREPARE GO HOME\n", request->example2);
        printk("urlmsg(u def)->msg %d IF SEE morethan 4096 PREPARE GO HOME\n", sizeof(request->msg));
        printk("urlmsg(u def)->msg %s IF SEE last 10 char PREPARE GO HOME\n", request->msg+sizeof(request->msg)-10);
    */

    // (struct pcn_kmsg_buf *buf(wrraper), int conn_no)
    // -------bottom half (deq) --------
    // (struct pcn_kmsg_buf *buf(wrraper), int conn_no)
    int err;

    // - alloc & cpy msg to kernel buffer
    struct pcn_kmsg_long_message *lmsg;
    lmsg = (struct pcn_kmsg_long_message*) pcn_kmsg_alloc_msg_long(cb->recv_buf.hdr.size);
    if (lmsg == NULL)
        printk("Unable to alloc a message\n");

    memcpy(lmsg, &cb->recv_buf, cb->recv_buf.hdr.size);

    // - semaphore down empty!

    // - semaphore up full!

    // - since interrupt triggers different thread and data set one by one? we don't have to put a lock here

    // - dispatch to workers (like enq_()) to do callback

    // - compose arguments
    pcn_kmsg_work_t *kmsg_work = kmalloc(sizeof(pcn_kmsg_work_t), GFP_ATOMIC);
    if (kmsg_work) {
        // wrap the entire msg
        kmsg_work->lmsg = lmsg;

        // remember to case. Jack: TODO ask why this is correct (from larger->smaller ---send---> smaller->larger)
        INIT_WORK((struct work_struct *)kmsg_work,pcn_kmsg_handler_BottomHalf);
        queue_work(msg_handler, (struct work_struct *)kmsg_work);
    } else {
        printk("Failed to kmalloc work structure!\n");
    }


    return 0;
}

static int krping_run_client(struct krping_cb *cb)
{
    struct ib_recv_wr *bad_wr;
    int ret;

    DEBUG_LOG("@@@ %s(): cb->conno %d\n", __func__, cb->conn_no);
    ret = krping_bind_client(cb);
    if (ret)
        return ret;

    ret = krping_setup_qp(cb, cb->cm_id);
    if (ret) {
        printk(KERN_ERR PFX "setup_qp failed: %d\n", ret);
        return ret;
    }

    ret = krping_setup_buffers(cb);
    if (ret) {
        printk(KERN_ERR PFX "krping_setup_buffers failed: %d\n", ret);
        goto err1;
    }

    DEBUG_LOG("ib_post_recv(manually)<<<<\n");
    ret = ib_post_recv(cb->qp, &cb->rq_wr, &bad_wr);
    if (ret) {
        printk(KERN_ERR PFX "ib_post_recv failed: %d\n", ret);
        goto err2;
    }

    ret = krping_connect_client(cb);
    if (ret) {
        printk(KERN_ERR PFX "connect error %d\n", ret);
        goto err2;
    }
    /*
    if (cb->wlat) {
        DEBUG_LOG("TEST client: krping_wlat_test_client()\n");
        krping_wlat_test_client(cb);
    }
    else if (cb->rlat) {
        DEBUG_LOG("TEST client: krping_rlat_test_client()\n");
        krping_rlat_test_client(cb);
    }
    else if (cb->bw) {
        DEBUG_LOG("TEST client: krping_bw_test_client()\n");
        krping_bw_test_client(cb);
    }
    else if (cb->frtest) {
        DEBUG_LOG("TEST client: krping_fr_test()\n");
        krping_fr_test(cb);
    }
    else {
        DEBUG_LOG("\n\n\n\n\nTEST client: krping_test_client()\n");
        krping_test_client(cb);
    }
    */

    return 0;

    //TODO: copy to outside the function
    rdma_disconnect(cb->cm_id); // used for rmmod
err2:
    krping_free_buffers(cb);
err1:
    krping_free_qp(cb);
    return ret;
}

// Initialize callback table to null, set up control and data channels
int __init initialize()
{
    int i, err, conn_no;
    char *tmp_net_dev_name;
    // Jack TODO: check how to assign a priority to these threads! make msg_layer faster (higher prio)
	// struct sched_param param = {.sched_priority = 10};
	MSGPRINTK("--- Popcorn messaging layer init starts ---\n");

    /* Register softirq handler */
    KRPRINT_INIT("Registering softirq handler (BottomHalf)...\n");
    //open_softirq(PCN_KMSG_SOFTIRQ, pcn_kmsg_action); // Jack TODO: check open_softirq()
    msg_handler = create_workqueue("msg_handler_BottomHalf"); // per-cpu

    for(i=0; i<MAX_NUM_NODES; i++) {
        // find my name
        if ( get_host_ip(tmp_net_dev_name) == ip_table2[i])  {
            my_cpu=i;
            MSGPRINTK("Device \"%s\" my_cpu=%d on machine IP %u.%u.%u.%u\n",
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
        MSGPRINTK(KERN_ERR "Jackmsglayer: ERROR ERROR ERROR\n");

    smp_mb(); // Jack: since my_cpu is extern (global)
	MSGPRINTK("----------------------------------------------------------\n");
	MSGPRINTK("------ updating to my_cpu=%d wait for a moment -----------\n", my_cpu);
	MSGPRINTK("----------------------------------------------------------\n");
	MSGDPRINTK("MSG_LAYER: Initialization my_cpu=%d\n", my_cpu);

	for (i = 0; i<MAX_NUM_NODES; i++) {
        // Jack: connection lable buf
        is_connection_done[i]=PCN_CONN_WATING; // connection status buf (each slot refers to each port)
	}

	/* Initilaize the IB */
    /*
     *  Each node has a connection table like tihs:
     * -----------------------------------------------------------------------------
     * | connect | connect | (many)... | my_cpu(one) | accept | accept | (many)... |
     * -----------------------------------------------------------------------------
     * my_cpu:  no need to talk to itself
     * connect: connecting to existing nodes
     * accept:  waiting for the connection requests from later nodes
     */
    for (i=0; i<MAX_NUM_NODES; i++) {
        conn_no=i;

        // 0. create and save to list
        //cb[i] = kzalloc(sizeof(*cb), GFP_KERNEL); // TODO: release
        cb[i] = kzalloc(sizeof(struct krping_cb), GFP_KERNEL); // TODO: release

        mutex_lock(&krping_mutex);
        list_add_tail(&cb[i]->list, &krping_cbs); // multiple cb!!!
        mutex_unlock(&krping_mutex);

        // settup node number
        cb[i]->conn_no = i;

        // setup ip
        //memcpy(cb[i]->addr, ip_table[i], sizeof(ip_table[i]));

        // setup locks
        mutex_init(&cb[i]->send_mutex);

        // 1. init comment parameters //
        cb[i]->state = IDLE; // init state!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
        cb[i]->size = 4*1024*1024; // Max transmission (buf) size // TODO: don't hardcode as this
        init_waitqueue_head(&cb[i]->sem); // init waitq for wait, wake up
        //cb[i]->txdepth = RPING_SQ_DEPTH; // ?
        cb[i]->txdepth = 64; // ? // TODO: don't hardcoded as this
        cb[i]->from_size = 4096; // Real transmission size // TODO: don't harcoded as this

        // 2. init uncomment parameters //
        // server/client/myself
        cb[i]->server = -1; // -1: myself

        // set up IPv4 address
        // Jack this is done by the function

        cb[i]->addr_str = ip_table[conn_no];
        //memcpy(cb[i]->addr_str, ip_table[i], sizeof(ip_table[i]));
        in4_pton(ip_table[conn_no], -1, cb[i]->addr, -1, NULL); // will be used
        cb[i]->addr_type = AF_INET; // [IPv4]/V6 // for determining
        cb[i]->port = htons(PORT); // always the same port
        KRPRINT_INIT("ipaddr %s port %d\n", ip_table[conn_no], (int)PORT);
        KRPRINT_INIT("ip_table[conn_no] %s, cb[i]->addr_str %s, cb[i]->addr %s,  port %d\n",
                            ip_table[conn_no], cb[i]->addr_str, cb[i]->addr, (int)PORT);

        // register event handler
        cb[i]->cm_id = rdma_create_id(&init_net, krping_cma_event_handler, cb[i], RDMA_PS_TCP, IB_QPT_RC);
        if (IS_ERR(cb[i]->cm_id)) {
            err = PTR_ERR(cb[i]->cm_id);
            printk(KERN_ERR PFX "rdma_create_id error %d\n", err);
            goto out;
        }
        KRPRINT_INIT("created cm_id %p (event handler)\n", cb[i]->cm_id);

    }
    smp_mb();
    KRPRINT_INIT("-------------- main init done (still cannot send/recv)-------------------\n\n");

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
    for (i=0; i<MAX_NUM_NODES; i++) { // TODO: May this order makes a DEADLOCK??
        if (i==my_cpu) {
            is_connection_done[i]=1;
            continue;
        }

        conn_no = i;     // Take node 1 for example. connect0 [1] accept2
        if (conn_no < my_cpu) { // 1. [connect] , my_cpu, accept
            cb[conn_no]->server = 0;
            // server/client dependant init
            msleep(5000);
            err = krping_run_client(cb[conn_no]); // connect_to()
            if (err){
                printk("WRONG!!\n"); return err;
            }

            is_connection_done[conn_no] = 1; //Jack: atomic is more safe
            smp_mb(); // Jack: just call it one time in the end, it should be fine
        } else if ( conn_no > my_cpu ) { // 2. connect , my_cpu, [accept]
            cb[conn_no]->server = 1;
            // server/client dependant init
            err = krping_run_server(cb[conn_no]); // accept();
            if (err){
                printk("WRONG!!\n"); return err;
            }

            is_connection_done[conn_no] = 1; //Jack: atomic is more safe
            smp_mb(); // Jack: just call it one time in the end, it should be fine
        }

        // TODO: Jack: use kthread run?
        //sched_setscheduler(<kthread_run()'s return>, SCHED_FIFO, &param);
		//set_cpus_allowed_ptr(<kthread_run()'s return>, cpumask_of(i%NR_CPUS));
    }

    for ( i=0; i<MAX_NUM_NODES; i++ ) {
        while ( is_connection_done[i]==PCN_CONN_WATING ) { //Jack: atomic is more safe
                MSGDPRINTK("waiting for is_connection_done[%d]\n", i);
                msleep(1000);
        }
    }
/*
    ///////////////////////////////////extra
        // send procedures
        int g_test_size=10;
        //cb->state = RDMA_READ_ADV;
        memset(cb->start_buf, 'j', sizeof(g_test_size));
        krping_format_send(cb, cb->rdma_dma_addr);
        ret = ib_post_send(cb->qp, &cb->sq_wr, &bad_wr); // sq_wr is hardcoded used for send&recv, rdma_sq_wr for W/R
        wait_event_interruptible(cb->sem, // sice thist // I don't have to have enq() deq()
                                cb->state >= RDMA_WRITE_COMPLETE);

        ib_post_send(cb->qp, &cb->sq_wr, &bad_wr); // qp, op(not really key), empty
        // &cb->sq_wr set up in if not using invalidation it will never change. Just keep sending
        // //Jack? where should I load buffer
        cb->sq_wr.opcode = IB_WR_SEND; // normal send / recv // harcoded (default)!!!
        cb->sq_wr.send_flags = IB_SEND_SIGNALED;
        cb->sq_wr.sg_list = &cb->send_sgl; // sge
        cb->sq_wr.num_sge = 1;

    ////////////////////////////////////////////////////////////
*/

	send_callback = (send_cbftn) ib_kmsg_send_long;
	smp_mb();
    MSGPRINTK("--- Popcorn messaging layer is up ---\n");

    /* Make init popcorn call */
	//_init_RemoteCPUMask(); // msg boradcast //Jack: deal w/ it later

    MSGPRINTK("--- Popcorn messaging self testing ---\n");
    //Jack TODO: move the following testing code to another place
    // register callback. also define in <linux/pcn_kmsg.h>
    pcn_kmsg_register_callback(PCN_KMSG_TYPE_FIRST_TEST,  // ping -
                                handle_remote_thread_first_test_request);
    //pcn_kmsg_register_callback(PCN_KMSG_TYPE_FIRST_TEST_HANDLER, // pong - usually a pair but just simply test here
    //                            handle_remote_thread_first_test_response);
	smp_mb();
    msleep(5000); // waitng for everyone registering the callback

    // compose msg - define -> alloc -> essential msg header info
    remote_thread_first_test_request_t* request; // youTODO: make your own struct
    request = (remote_thread_first_test_request_t*) kmalloc(sizeof(remote_thread_first_test_request_t), GFP_ATOMIC);
    if(request==NULL)
        return -1;

    request->hdr.type = PCN_KMSG_TYPE_FIRST_TEST; // idx 0
    request->hdr.prio = PCN_KMSG_PRIO_NORMAL;
    //request->tgroup_home_cpu = tgroup_home_cpu;
    //request->tgroup_home_id = tgroup_home_id;

    // msg essentials
    //------------------------------------------------------------
    // msg dependences
    request->example1 = 1;
    request->example2 = 2;
    memset(request->msg,'J', sizeof(request->msg));
    MSGDPRINTK("testing msg size = strlen(request->msg) %d\n", strlen(request->msg));

    // send msg - broadcast // Jack TODO: compared with list_for_each_safe, which one is faster
    for(i=0; i<MAX_NUM_NODES; i++){
        if(my_cpu==i)
            continue;
        printk("Jack: test9 - dst %d\n", i);
        pcn_kmsg_send_long(i, (struct pcn_kmsg_long_message*) request,
            sizeof(remote_thread_first_test_request_t) - sizeof(struct pcn_kmsg_hdr));  //Jack: redundant calculation
    }

	MSGPRINTK("Jack's testing DONE! If see N-1 (good), muli-node msg_layer over ipoib is healthy!!!\n");
	MSGDPRINTK("Value of send ptr = %p\n", send_callback);

	MSGDPRINTK(KERN_INFO "Popcorn Messaging Layer Initialized\n");

    return 0;

out:
    for(i=0; i<MAX_NUM_NODES; i++){
        if(cb[i]->state!=0) { // making sure it hasbeen inited
            mutex_lock(&krping_mutex);
            list_del(&cb[i]->list);
            mutex_unlock(&krping_mutex);
            kfree(cb[i]);
            // TODO: cut connections
        }
    }
    return err;

}

// For testing
int pcn_kmsg_register_callback(enum pcn_kmsg_type type, pcn_kmsg_cbftn callback)
{
    if (type >= PCN_KMSG_TYPE_MAX)
        return -ENODEV; /* invalid type */

    MSGDPRINTK("%s: registering %d\n",__func__, type);
    callbacks[type] = callback;
    return 0;
}

/*
 * return the (possibly rebound) rkey for the rdma buffer.
 * REG mode: invalidate and rebind via reg wr.
 * other modes: just return the mr rkey.
 */
/*
static u32 krping_rdma_rkey(struct krping_cb *cb, u64 buf, int post_inv)
{
    u32 rkey;
    struct ib_send_wr *bad_wr;
    int ret;
    struct scatterlist sg = {0};

    cb->invalidate_wr.ex.invalidate_rkey = cb->reg_mr->rkey; //redundant if i diable post_inv

    //
    // Update the reg key.
    //
    //ib_update_fast_reg_key(cb->reg_mr, ++cb->key);
    ib_update_fast_reg_key(cb->reg_mr, cb->key); // Jack testing
    cb->reg_mr_wr.key = cb->reg_mr->rkey;

    //
    // Update the reg WR with new buf info.
    //
    if (buf == (u64)cb->start_dma_addr)
        cb->reg_mr_wr.access = IB_ACCESS_REMOTE_READ;
    else
        cb->reg_mr_wr.access = IB_ACCESS_REMOTE_WRITE | IB_ACCESS_LOCAL_WRITE;
    sg_dma_address(&sg) = buf;      // rdma_buf = rdma_buf
    //sg_dma_len(&sg) = cb->size; //TODO Jack does this dynamic change the send size !!!!!!
    //sg_dma_len(&sg) = 4096-1; //TODO Jack does this dynamic change the send size !!!!!printk("hardcoded size %d\n",  cb->siz);
    //sg_dma_len(&sg) = cb->from_size; //TODO Jack does this dynamic change the send size !!!!!!

    // Jack!!! support dynamically chage the R/W length
    //if(cb->server){
    //EXP_LOG("got the size from remote %d\n", cb->remote_len);
    //sg_dma_len(&sg) = cb->remote_len;
    //}
    //else {
    //    EXP_LOG("cb->from_size %d\n", cb->from_size);
    sg_dma_len(&sg) = cb->from_size; //TODO Jack does this dynamic change the send size !!!!!!
    //
    //}
    EXP_LOG("hardcoded (fixed) size %d\n",  sg_dma_len(&sg));

    //ret = ib_map_mr_sg(cb->reg_mr, &sg, 1, NULL, PAGE_SIZE);
    ret = ib_map_mr_sg(cb->reg_mr, &sg, 1, PAGE_SIZE);  // snyc ib_dma_sync_single_for_cpu/dev
    BUG_ON(ret <= 0 || ret > cb->page_list_len);

    DEBUG_LOG(PFX "post_inv = %d, reg_mr new rkey %d pgsz %u len %u"
        " iova_start %llx\n",
        post_inv,
        cb->reg_mr_wr.key,
        cb->reg_mr->page_size,
        cb->reg_mr->length,
        cb->reg_mr->iova);

    DEBUG_LOG("ib_post_send>>>>\n");
    if (post_inv)
        ret = ib_post_send(cb->qp, &cb->invalidate_wr, &bad_wr);
    else
        ret = ib_post_send(cb->qp, &cb->reg_mr_wr.wr, &bad_wr);
    if (ret) {
        printk(KERN_ERR PFX "post send error %d\n", ret);
        cb->state = ERROR;
    }
    rkey = cb->reg_mr->rkey; //
    return rkey;
}
*/

/*
static void krping_format_send(struct krping_cb *cb, u64 buf)
{
    struct krping_rdma_info *info = &cb->send_buf; // update send_buf
    u32 rkey;

    ///
     // Client side will do reg or mw bind before
     // advertising the rdma buffer.  Server side
     // sends have no data.
     //
    //if (!cb->server || cb->wlat || cb->rlat || cb->bw) { // only client!!!!
        rkey = krping_rdma_rkey(cb, buf, !cb->server_invalidate); //Jack failed to trun inv off
        info->buf = htonll(buf);            // update. hton: host to net order
        info->rkey = htonl(rkey);           // update
        //info->size = htonl(cb->size);       // update
        //info->size = htonl(4096-1);       // update //Jack!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!fix sock size
        info->size = htonl(cb->from_size);       // update //Jack !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!fix sock size
        //DEBUG_LOG("RDMA addr %llx rkey %d len %d\n",
        //      (unsigned long long)buf, rkey, cb->size);
        DEBUG_LOG("RDMA addr %llx rkey %d len %d\n",
              (unsigned long long)buf, rkey, 4096-1);
    //}
}
*/

int ib_kmsg_send_long(unsigned int dest_cpu, struct pcn_kmsg_long_message *lmsg, unsigned int payload_size) { // called by user or kernel
    printk("JACK: CONNECTIONS ESTABLISHED. KEEP WORKING\n");
    volatile int left;
    int size = 0, ret;
    struct ib_send_wr *bad_wr;

    lmsg->hdr.size = payload_size + sizeof(lmsg->hdr); // total size
    MSGDPRINTK("%s() - payload_size %d sizeof(lmsg->hdr) %d\n", __func__, payload_size, sizeof(lmsg->hdr));
    lmsg->hdr.from_cpu = my_cpu;

    // TODO check itslef
    if(dest_cpu==my_cpu) {
        MSGDPRINTK("Jackmsglayer: itself %d\n", dest_cpu);
        return 0;
    }

    // over MAX?
    MSGDPRINTK("%s() - MAX single msg size == sizeof(struct pcn_kmsg_long_message) %d\n",
                                           __func__, sizeof(struct pcn_kmsg_long_message));
    if( lmsg->hdr.size > sizeof(struct pcn_kmsg_long_message)){
        printk("%s(): ERROR - MSG %d larger than MAX_MSG_SIZE %d\n",
                    __func__, lmsg->hdr.size, sizeof(struct pcn_kmsg_long_message));
    }

    // pcn_msg (abstraction msg layer)
    //----------------------------------------------------------
    // ib

    // - down()
    mutex_lock(&cb[dest_cpu]->send_mutex);
    // copy form kernel buf to ib buf (start_buf = send_buf!)
    memcpy(&cb[dest_cpu]->send_buf, lmsg, lmsg->hdr.size); // send msg including hdr - lmsg = hdr + payload // not the entire lmsg (p_k_long_msg)!!!

    ret = ib_post_send(cb[dest_cpu]->qp, &cb[dest_cpu]->sq_wr, &bad_wr); // sq_wr is hardcoded used for send&recv, rdma_sq_wr for W/R
    // - up()

    //wait_event_interruptible(cb[dest_cpu]->sem, // sice thist // I don't have to have enq() deq()
    //                        cb[dest_cpu]->state >= RDMA_SEND_COMPLETE);
    // TODO: SEND AND GO NOW (NO CHECKING)!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!

    MSGDPRINTK("Jackmsglayer: 1 msg snet through dest_cpu %d\n", dest_cpu);
    return 0;
}


// TODO: clean sock parts, add ib parts
static void __exit unload(void)
{
	int i;
    MSGPRINTK("Stopping kernel threads\n");
    /** TODO: at least a NULL a pointer below this line **/
	/* To move out of recv_queue */
	for (i = 0; i<MAX_NUM_NODES; i++) {
		//complete_all(&send_completion[i]);
		//complete_all(&recv_completion[i]);

        //sema_destroy(&connect_sem[i]); // api not found
        //sema_destroy(&accept_sem[i]); // api not found
	}
	msleep(100);

	/* release */
    MSGPRINTK("Release threadss\n");
	for (i = 0; i<MAX_NUM_NODES; i++) {
        //if(handler[i]!=NULL)
        //    kthread_stop(handler[i]);
        //if(sender_handler[i]!=NULL)
        //    kthread_stop(sender_handler[i]);
        //if(execution_handler[i]!=NULL)
        //    kthread_stop(execution_handler[i]);
        //Jack: TODO: sock release buffer, check(according to) the init
    }

    //TODO: release socket
    //MSGPRINTK("Release sockets\n");
    //sock_release(sock_listen);
	for (i = 0; i<MAX_NUM_NODES; i++) {
        //if(sock_data[i]!=NULL) // TODO: see if we need this
        //    sock_release(sock_data[i]);
    }
    //sock_release(sock_listen);

    MSGPRINTK("Successfully unloaded module!\n");
}

module_init(initialize);
module_exit(unload);
MODULE_LICENSE("GPL");
