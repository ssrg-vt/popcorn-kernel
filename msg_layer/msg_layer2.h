/*
 * msg_layer2.h
 * Copyright (C) 2017 jackchuang <jackchuang@echo3>
 *
 * Distributed under terms of the MIT license.
 */

#ifndef MSG_LAYER2_H
#define MSG_LAYER2_H
#include <rdma/ib_verbs.h>
#include <rdma/rdma_cm.h>

// (temporary) TODO: move all def to this bundle.h
#include <popcorn/bundle.h>

/* Machines info !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!! */ 
#define MAX_NUM_NODES       3  // total num of machines
#define MAX_NUM_CHANNELS    MAX_NUM_NODES-1 //=MAX

#define htonll(x) cpu_to_be64((x))
#define ntohll(x) cpu_to_be64((x)) 

// global
#define MSGDEBUG 1
#define DEBUG 1
#define DEBUG_VERBOSE 1
#define KRPING_EXP_LOG 1
#define KRPING_EXP_DATA 0
#define FORCE_DEBUG 1
#define MSG_SYNC_DEBUG 0
#define MSG_RDMA_DEBUG 0

/* perf data */
#define EXP_DATA if(KRPING_EXP_DATA) printk
/* perf log */
#define EXP_LOG if(KRPING_EXP_LOG) printk 

/* special debug log */
#define MSG_SYNC_PRK if(MSG_SYNC_DEBUG) printk
#define MSG_RDMA_PRK if(MSG_RDMA_DEBUG) printk
#define KRPRINT_INIT printk

/* normal debug log */
#define DEBUG_LOG if(FORCE_DEBUG || (DEBUG && !KRPING_EXP_DATA)) printk
#define DEBUG_LOG_V if(DEBUG_VERBOSE) trace_printk 



/* ib specific */
#ifdef CONFIG_POPCORN_KMSG_IB
//#define MAX_RECV_WR 10
//#define MAX_RECV_WR 15
// MAX =16, size = kmsg_size = 4*4*1024
//#define MAX_RECV_WR 16
#define MAX_RECV_WR 500
//#define MAX_RECV_WR 16

// rdma size
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
    int server;         /* 0 iff client */
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

    //struct ib_recv_wr rq_wr[MAX_RECV_WR];     /* recv work request record */
    //struct ib_sge recv_sgl[MAX_RECV_WR];      /* recv single SGE */
    //struct pcn_kmsg_long_message recv_buf[MAX_RECV_WR]; /* malloc'd buffer */ /* msg unit[] */
    int recv_size;

    u64 recv_dma_addr;
    //DECLARE_PCI_UNMAP_ADDR(recv_mapping) // cannot compile = =
    u64 recv_mapping;

    struct ib_send_wr sq_wr;    /* send work requrest record */
    struct ib_sge send_sgl;
    struct pcn_kmsg_long_message send_buf;  /* single send buf */ /* msg unit */
    u64 send_dma_addr;
    //DECLARE_PCI_UNMAP_ADDR(send_mapping) // cannot compile = =
    u64 send_mapping;

    struct ib_rdma_wr rdma_sq_wr;   /* rdma work request record */
    struct ib_sge rdma_sgl;         /* rdma single SGE */
   
    /* a rdma buf for active */
    char *rdma_buf;         /* used as rdma sink */
    u64  rdma_dma_addr;     /* for active buffer */
    //DECLARE_PCI_UNMAP_ADDR(rdma_mapping) // cannot compile = =
    u64 rdma_mapping;
    struct ib_mr *rdma_mr;

    uint32_t remote_rkey;       /* remote guys RKEY */
    uint64_t remote_addr;       /* remote guys TO */
    uint32_t remote_len;        /* remote guys LEN */

    /* a rdma buf for passive */ 
    char *passive_buf;          /* passive R/W buffer */
    u64  passive_dma_addr;      /* passive R/W buffer addr*/
    //DECLARE_PCI_UNMAP_ADDR(start_mapping) // cannot compile = =
    u64 start_mapping;
    struct ib_mr *start_mr;

    //enum test_state state;    /* used for cond/signalling */
    atomic_t state;             
    atomic_t send_state;        
    atomic_t recv_state;        
    atomic_t read_state;        
    atomic_t write_state;       
    //atomic_t irq;             
    wait_queue_head_t sem;
    struct krping_stats stats;

    uint16_t port;          /* dst port in NBO */
    u8 addr[16];            /* dst addr in NBO */
    char *addr_str;         /* dst addr string */
    uint8_t addr_type;      /* ADDR_FAMILY - IPv4/V6 */
    int verbose;            /* verbose logging */
    int count;              /* ping count */
    //int size;             /* ping data size */
    unsigned long rdma_size;    /* ping data size */
    int validate;               /* validate ping data */
    int wlat;               /* run wlat test */
    int rlat;               /* run rlat test */
    int bw;                 /* run bw test */
    int duplex;             /* run bw full duplex test */
    int poll;               /* poll or block for rlat test */
    int txdepth;            /* SQ depth */
    int local_dma_lkey;     /* use 0 for lkey */
    int frtest;             /* reg test */

    /* CM stuff */
    struct rdma_cm_id *cm_id;       /* connection on client side,*/
                                    /* listener on server side. */
    struct rdma_cm_id *child_cm_id; /* connection on server side */
    struct list_head list;
    int conn_no;
    
    /* sync */
    struct mutex send_mutex;
    struct mutex recv_mutex;
    struct mutex active_mutex;  
    struct mutex passive_mutex; /* passive lock*/
    struct mutex qp_mutex;      /* protect ib_post_send(qp)*/
    atomic_t active_cnt;        /* used for cond/signalling */
    atomic_t passive_cnt;       /* used for cond/signalling */
   
    /* for sync problem */
    spinlock_t rw_slock;
    atomic_t g_all_ticket;
    atomic_t g_now_ticket;
    
    atomic_t g_wr_id;           // for assigning wr_id
};

/* utilities */
u32 krping_rdma_rkey(struct krping_cb *cb, u64 buf, int post_inv, int rdma_len);
u32 krping_rdma_rkey_passive(struct krping_cb *cb, u64 buf, int post_inv, int rdma_len);

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
#endif
/* global */
//extern int my_cpu;
extern pcn_kmsg_cbftn callbacks[PCN_KMSG_TYPE_MAX];

#endif /* !MSG_LAYER2_H */
