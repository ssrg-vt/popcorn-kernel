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

/*This Set of macros allows for forwarding of syscalls of up to 6 arguments,
 *with 12 arguments being input altogether, eg. SET_REQ_PARAMS(int, a, char, b)
 *This segment will give the  */
#define _PARAM_0_VAL(arg0)	req->param0 = (uint64_t)arg0;
#define _PARAM_0_TYPE(type, ...) _PARAM_0_VAL(__VA_ARGS__)
#define _PARAM_1_VAL(arg1, ...) req->param1 = (uint64_t)arg1; _PARAM_0_TYPE(__VA_ARGS__)
#define _PARAM_1_TYPE(type, ...) _PARAM_1_VAL(__VA_ARGS__)
#define _PARAM_2_VAL(arg2, ...) req->param2 = (uint64_t)arg2; _PARAM_1_TYPE(__VA_ARGS__)
#define _PARAM_2_TYPE(type, ...) _PARAM_2_VAL(__VA_ARGS__)
#define _PARAM_3_VAL(arg3, ...) req->param3 = (uint64_t)arg3; _PARAM_2_TYPE(__VA_ARGS__)
#define _PARAM_3_TYPE(type, ...) _PARAM_3_VAL(__VA_ARGS__)
#define _PARAM_4_VAL(arg4, ...) req->param4 = (uint64_t)arg4; _PARAM_3_TYPE(__VA_ARGS__)
#define _PARAM_4_TYPE(type, ...) _PARAM_4_VAL(__VA_ARGS__)
#define _PARAM_5_VAL(arg5, ...) req->param5 = (uint64_t)arg5; _PARAM_4_TYPE(__VA_ARGS__)
#define _PARAM_5_TYPE(type, ...) _PARAM_5_VAL(__VA_ARGS__)

#define INVALID_ARGUMENTS(...) return -EINVAL;

#define _SET_REQ_PARAMS(_1, _2, _3, _4, _5, _6, _7, _8, _9, _10, _11, _12, \
			NAME, ...) NAME
#define SET_REQ_PARAMS_ARGS(...)						\
	_SET_REQ_PARAMS(__VA_ARGS__, _PARAM_5_TYPE, INVALID_ARGUMENTS,		\
			_PARAM_4_TYPE, INVALID_ARGUMENTS,			\
			_PARAM_3_TYPE, INVALID_ARGUMENTS,			\
			_PARAM_2_TYPE, INVALID_ARGUMENTS,			\
			_PARAM_1_TYPE, INVALID_ARGUMENTS,			\
			_PARAM_0_TYPE, INVALID_ARGUMENTS,			\
			)(__VA_ARGS__)

/* Macros to change varargs (up to 6) with commas between types into regular
 * args. Eg. DEFINE_SYSCALL_REDIRECT(write, int, a, char, b) becomes
 * redirect_write(int a, char b)*/
#define _ARG_0_VAL(arg0)	arg0
#define _ARG_0_TYPE(type, ...)	type _ARG_0_VAL(__VA_ARGS__)
#define _ARG_1_VAL(arg1, ...)   arg1, _ARG_0_TYPE(__VA_ARGS__)
#define _ARG_1_TYPE(type, ...)  type _ARG_1_VAL(__VA_ARGS__)
#define _ARG_2_VAL(arg2, ...)   arg2, _ARG_1_TYPE(__VA_ARGS__)
#define _ARG_2_TYPE(type, ...)  type _ARG_2_VAL(__VA_ARGS__)
#define _ARG_3_VAL(arg3, ...)   arg3, _ARG_2_TYPE(__VA_ARGS__)
#define _ARG_3_TYPE(type, ...)  type _ARG_3_VAL(__VA_ARGS__)
#define _ARG_4_VAL(arg4, ...)   arg4, _ARG_3_TYPE(__VA_ARGS__)
#define _ARG_4_TYPE(type, ...)  type _ARG_4_VAL(__VA_ARGS__)
#define _ARG_5_VAL(arg5, ...)	arg5, _ARG_4_TYPE(__VA_ARGS__)
#define _ARG_5_TYPE(type, ...)	type _ARG_5_VAL(__VA_ARGS__)

#define _LIST_ARGS(_1, _2, _3, _4, _5, _6, _7, _8, _9, _10, _11, _12, \
			NAME, ...) NAME
#define LIST_SYSCALL_ARGS(...)						\
	_LIST_ARGS(__VA_ARGS__, _ARG_5_TYPE, INVALID_ARGUMENTS,		\
			_ARG_4_TYPE, INVALID_ARGUMENTS,			\
			_ARG_3_TYPE, INVALID_ARGUMENTS,			\
			_ARG_2_TYPE, INVALID_ARGUMENTS,			\
			_ARG_1_TYPE, INVALID_ARGUMENTS,			\
			_ARG_0_TYPE, INVALID_ARGUMENTS,			\
			)(__VA_ARGS__)


#define DEFINE_SYSCALL_REDIRECT(syscall,...)				\
DEFINE_PCN_KMSG(syscall_fwd_##syscall_t, _REMOTE_SYSCALL_ARGS(__VA_ARGS__)); \
inline int redirect_##syscall(LIST_SYSCALL_ARGS(__VA_ARGS__)) {		\
	syscall_fwd_t *req = kmalloc(sizeof(syscall_fwd_t));		\
	syscall_rep_t *rep = NULL;					\
	struct wait_station *ws = get_wait_station(current);		\
	SET_REQ_PARAMS_ARGS(__VA_ARGS__)				\
									\
	retval = pcn_kmsg_send(PCN_KMSG_TYPE_SYSCALL_FWD, 0, req,	\
			    sizeof(*req));				\
	kfree(req);							\
	reply = wait_at_station(ws);					\
	retval = reply->retval;						\
	/*SKPRINTK("reply from master: %d\n", retval);*/		\
	return retval;							\
}

