// SPDX-License-Identifier: GPL-2.0, BSD
#ifndef _POPCORN_WAIT_STATION_H_
#define _POPCORN_WAIT_STATION_H_

#include <linux/completion.h>
#include <linux/atomic.h>

struct wait_station {
	int id;
	pid_t pid;
	volatile void *private;
	struct completion pendings;
	atomic_t pendings_count;
};

struct task_struct;

struct wait_station *get_wait_station_multiple(struct task_struct *tsk,
					int count);
static inline struct wait_station *get_wait_station(struct task_struct *tsk)
{
	return get_wait_station_multiple(tsk, 1);
}
struct wait_station *wait_station(int id);
void put_wait_station(struct wait_station *ws);
void *wait_at_station(struct wait_station *ws);
#endif
