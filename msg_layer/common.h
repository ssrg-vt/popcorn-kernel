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

#define MAX_NUM_NODES		2	// For development convenience
#define MAX_NUM_CHANNELS	(MAX_NUM_NODES - 1)

/* put here since ib request handler and 
 * caller response handler(user defined) will both need.
 */
typedef struct {
	struct pcn_kmsg_hdr header; /* must followd */
	/* rdma essential */
	bool is_rdma;
	bool is_write;
	bool rdma_ack;			/* passive side acks in the end of request */
	uint32_t remote_rkey;	/* R/W remote RKEY (body) */
	uint32_t rw_size;		/* R/W remote size (body) */
	uint64_t remote_addr;	/* remote TO (body) */ 
	void *your_buf_ptr;		/* will be copied to R/W buffer (body) */
#ifdef CONFIG_POPCORN_DEBUG_MSG_LAYER_VERBOSE
	int rw_ticket;
	int rdma_ticket;
#endif	
}__attribute__((packed)) remote_thread_rdma_rw_request_t;

#endif
