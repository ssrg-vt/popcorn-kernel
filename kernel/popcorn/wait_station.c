/**
 * Waiting stations allows threads to be waited for a given 
 * number of events are completed
 */
#include <linux/kernel.h>
#include <linux/sched.h>

#include "wait_station.h"

#define MAX_WAIT_STATIONS 64

static struct wait_station wait_stations[MAX_WAIT_STATIONS];

static DEFINE_SPINLOCK(wait_station_lock);
static DECLARE_BITMAP(wait_station_available, MAX_WAIT_STATIONS) = { 0 };

struct wait_station *get_wait_station_multiple(struct task_struct *tsk, int count)
{
	int id;
	struct wait_station *ws;

	spin_lock(&wait_station_lock);
	id = find_first_zero_bit(wait_station_available, MAX_WAIT_STATIONS);
	ws = wait_stations + id;
	set_bit(id, wait_station_available);
	spin_unlock(&wait_station_lock);

	ws->id = id;
	ws->pid = tsk->pid;
	ws->private = NULL;
	init_completion(&ws->pendings);
	atomic_set(&ws->pendings_count, count);
	smp_wmb();
	//printk(" *[%d]: %d allocated\n", ws->pid, id);

	return ws;
}
EXPORT_SYMBOL_GPL(get_wait_station_multiple);

struct wait_station *wait_station(int id)
{
	smp_rmb();
	return wait_stations + id;
}
EXPORT_SYMBOL_GPL(wait_station);

void put_wait_station(struct wait_station *ws)
{
	int id = ws->id;
	spin_lock(&wait_station_lock);
	BUG_ON(!test_bit(id, wait_station_available));
	clear_bit(id, wait_station_available);
	spin_unlock(&wait_station_lock);
	//printk(" *[%d]: %d returned\n", ws->pid, id);
}
EXPORT_SYMBOL_GPL(put_wait_station);

void *wait_at_station(struct wait_station *ws)
{
	if (!try_wait_for_completion(&ws->pendings)) {
		wait_for_completion(&ws->pendings);
	}
	return (void *)ws->private;
}
EXPORT_SYMBOL_GPL(wait_at_station);
