/**
 * Header file for Popcorn remote syscall protocol
 *
 *     SengMing Yeoh <sengming@vt.edu> 2018
 */

#ifndef __POPCORN_PCN_KMSG_H__
#define __POPCORN_PCN_KMSG_H__

#include <linux/unistd.h>
#include <popcorn/pcn_kmsg.h>
#include <popcorn/types.h>
#include <popcorn/debug.h>
#include "wait_station.h"
#include "types.h"


#define PARAM_0(arg0)	   req->param0 = (uint64_t)arg0;
#define PARAM_1(arg1, ...) req->param1 = (uint64_t)arg1; PARAM_0(__VA_ARGS__)
#define PARAM_2(arg2, ...) req->param2 = (uint64_t)arg2; PARAM_1(__VA_ARGS__)
#define PARAM_3(arg3, ...) req->param3 = (uint64_t)arg3; PARAM_2(__VA_ARGS__)
#define PARAM_4(arg4, ...) req->param4 = (uint64_t)arg4; PARAM_3(__VA_ARGS__)
#define PARAM_5(arg5, ...) req->param5 = (uint64_t)arg5; PARAM_4(__VA_ARGS__)

#define _SET_REQ_PARAMS(_1, _2, _3, _4, _5, _6, NAME, ...) NAME
#define SET_REQ_PARAMS_ARGS(...)						\
	_SET_REQ_PARAMS(__VA_ARGS__, PARAM_5, PARAM_4, PARAM_3,	\
			PARAM_2, PARAM_1, PARAM_0)(__VA_ARGS__)	\

#define DEFINE_SYSCALL_REDIRECT(syscall,...) \
DEFINE_PCN_KMSG(syscall_fwd_##syscall_t, _REMOTE_SYSCALL_ARGS(__VA_ARGS__)); \
inline int redirect_##syscall(__VA_ARGS__) {				\
	syscall_fwd_t *req = kmalloc(sizeof(syscall_fwd_t))		\
	syscall_rep_t *rep = NULL;					\
	struct wait_station *ws = get_wait_station(current);		\
	SET_REQ_PARAMS_ARGS(__VA_ARGS__);				\
									\
	retval = pcn_kmsg_send(PCN_KMSG_TYPE_SYSCALL_FWD, 0, req,	\
			    sizeof(*req));				\
	kfree(req);							\
	reply = wait_at_station(ws);					\
	retval = reply->retval;						\
	/*SKPRINTK("reply from master: %d\n", retval);*/		\
	return retval;							\
}

