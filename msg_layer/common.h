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

#include "config.h"

/* Message usage pattern */
#ifdef CONFIG_POPCORN_MSG_STATISTIC
extern atomic_t send_pattern[];
extern atomic_t recv_pattern[];
#endif

#endif
