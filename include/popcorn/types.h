/**
 * @file include/popcorn/types.h
 *
 * Define constant variables and define optional features
 *
 * @author Sang-Hoon Kim, SSRG Virginia Tech, 2017
 * @author Marina Sadini, SSRG Virginia Tech, 2013
 */

#ifndef __INCLUDE_POPCORN_TYPES_H__
#define __INCLUDE_POPCORN_TYPES_H__

#include <linux/sched.h>

static inline bool distributed_process(struct task_struct *tsk)
{
	if (!tsk->mm) return false;
	return !!tsk->mm->remote;
}

static inline bool distributed_remote_process(struct task_struct *tsk)
{
	return distributed_process(tsk) && tsk->at_remote;
}

#include <popcorn/debug.h>

#endif /* __INCLUDE_POPCORN_TYPES_H__ */
