/**
 * Header file for Popcorn remote syscall protocol
 *
 *     SengMing Yeoh <sengming@vt.edu> 2018
 */

#ifndef __POPCORN_SYSCALL_FWD_H__
#define __POPCORN_SYSCALL_FWD_H__

#include <linux/unistd.h>
#include <popcorn/pcn_kmsg.h>
#include <popcorn/types.h>
#include <popcorn/debug.h>
#include "types.h"

//int process_remote_syscall(struct pcn_kmsg_message *msg);
//int handle_signal_remotes(struct pcn_kmsg_message  *msg);
long syscall_redirect(unsigned long nr, struct pt_regs *regs);
/*This Set of macros allows for forwarding of syscalls of up to 6 arguments,
 *with 12 arguments being input altogether, eg. SET_REQ_PARAMS(int, a, char, b)
 *This segment will fill in the RPC syscall definitions with the correct
 *params from 1 to 6. This is filled in backwards due to the way we're
 *implementing the macros, so when you call SET_REQ_PARAM_ARGS, based on the
 *VA_ARGS number it offsets into _SET_REQ_PARAMS which will call a particular
 *_PARAM_X_TYPE which then recursively expands the PARAMS before it. If we have
 *an odd number of arguments we get a return -EINVAL instead since we need
 *type-value pairs*/
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

/* Counts the number of arguments using an offset into _LIST_ARGS */
#define NUM_ARGS(...) _LIST_ARGS(__VA_ARGS__,12,11,10,9,8,7,6,5,4,3,2,1,0)

#define LIST_SYSCALL_ARGS(...)						\
	_LIST_ARGS(__VA_ARGS__, _ARG_5_TYPE, INVALID_ARGUMENTS,		\
			_ARG_4_TYPE, INVALID_ARGUMENTS,			\
			_ARG_3_TYPE, INVALID_ARGUMENTS,			\
			_ARG_2_TYPE, INVALID_ARGUMENTS,			\
			_ARG_1_TYPE, INVALID_ARGUMENTS,			\
			_ARG_0_TYPE, INVALID_ARGUMENTS,			\
			)(__VA_ARGS__)

/* Reverses arg pairs, so if you call REVERSE(int, a, char, b) it will
 * return char, b, int, a. We use this to undo the reversing effect of
 * SET_REQ_PARAMS. REVERSE needs to call REVERSE1 which seemingly does nothing
 * because of the way macro expansion works. REVERSE takes N as an argument for
 * the number of arguments to be reversed, but we can only get that by using the
 * NUM_ARGS macro, so for it to expand correctly we need to call it like so:
 * REVERSE(NUM_ARGS(__VA_ARGS__), __VA_ARGS__)*/
#define REVERSE_2(a, b) a, b
#define REVERSE_4(a,b,...) REVERSE_2(__VA_ARGS__),a, b
#define REVERSE_6(a,b,...) REVERSE_4(__VA_ARGS__),a, b
#define REVERSE_8(a,b,...) REVERSE_6(__VA_ARGS__),a, b
#define REVERSE_10(a,b,...) REVERSE_8(__VA_ARGS__),a, b
#define REVERSE_12(a,b,...) REVERSE_10(__VA_ARGS__),a, b
#define REVERSE1(N,...) REVERSE_ ## N(__VA_ARGS__)
#define REVERSE(N, ...) REVERSE1(N, __VA_ARGS__)



#endif
