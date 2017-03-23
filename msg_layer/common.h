/*
 * msg_layer2.h
 * Copyright (C) 2017 jackchuang <jackchuang@echo3>
 *
 * Distributed under terms of the MIT license.
 */

#ifndef MSG_LAYER2_H
#define MSG_LAYER2_H
#include <popcorn/debug.h>
#include <popcorn/bundle.h>
#include <popcorn/pcn_kmsg.h>

#include <rdma/rdma_cm.h>

#define MAX_NUM_NODES       2  // total num of machines
#define MAX_NUM_CHANNELS    MAX_NUM_NODES-1 //=MAX

// TODO: replace with
//MSGPRINTK(...)
//MSGDPRINTK(...)
//MSGDATA(...)
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

#endif /* !MSG_LAYER2_H */
