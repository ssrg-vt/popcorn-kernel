/*
 * common.h
 * Copyright (C) 2017 jackchuang <jackchuang@echo3>
 *
 * Distributed under terms of the MIT license.
 */

#ifndef _MSG_LAYER_COMMON_H_
#define _MSG_LAYER_COMMON_H_

#include <popcorn/pcn_kmsg.h>
#include <popcorn/bundle.h>
#include <popcorn/debug.h>

#define MAX_NUM_NODES		2
#define MAX_NUM_CHANNELS	(MAX_NUM_NODES - 1)

//#ifdef CONFIG_POPCORN_KMSG_IB
// put here since ib request handler and caller response handler(user defined) will both need
// TODO: make as one r/w struct
typedef struct {                                                                                                                                    
    //struct pcn_kmsg_rdma_header header; /* must followd */
    struct pcn_kmsg_hdr header; /* must followd */
    /* you define */
    uint32_t remote_rkey;   /* R/W remote RKEY (body) */
    uint32_t rdma_size;     /* R/W remote size (body) */ //TODO change to r/w size
    uint64_t remote_addr;   /* remote TO (body) */ 
    void *your_buf_ptr;     /* will be copied to R/W buffer (body) */
}__attribute__((packed)) remote_thread_rdma_read_request_t; // for cache

typedef struct {
    //struct pcn_kmsg_rdma_header header; /* must followd */
    struct pcn_kmsg_hdr header; /* must followd */
    /* you define */
    uint32_t remote_rkey;   /* R/W remote RKEY (body) */ 
    uint32_t rdma_size;     /* R/W remote size (body) */
    uint64_t remote_addr;   /* remote TO (body) */ 
    void *your_buf_ptr;     /* will be copied to R/W buffer (body) */
}__attribute__((packed)) remote_thread_rdma_write_request_t; // for cache
//#endif



#endif
