/**
 * Waiting stations allows threads to be waited for a given 
 * number of events are completed
 */

#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/err.h>

#include "wait_station.h"

#define MAX_WAIT_STATIONS 1024

static struct wait_station wait_stations[MAX_WAIT_STATIONS];

static DEFINE_SPINLOCK(wait_station_lock);
static DECLARE_BITMAP(wait_station_available, MAX_WAIT_STATIONS) = { 0 };

struct wait_station *get_wait_station_multiple(struct task_struct *tsk, int count)
{
	int id;
	struct wait_station *ws;

	spin_lock(&wait_station_lock);
	id = find_first_zero_bit(wait_station_available, MAX_WAIT_STATIONS);
	BUG_ON(id >= MAX_WAIT_STATIONS);
	ws = wait_stations + id;
	set_bit(id, wait_station_available);
	spin_unlock(&wait_station_lock);

	ws->id = id;
	ws->pid = tsk->pid;
	ws->private = (void *)0xbad0face;
	init_completion(&ws->pendings);
	atomic_set(&ws->pendings_count, count);
	smp_wmb();

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
}
EXPORT_SYMBOL_GPL(put_wait_station);

void *wait_at_station(struct wait_station *ws)
{
	void *ret;
	if (!try_wait_for_completion(&ws->pendings)) {
		if (wait_for_completion_io_timeout(&ws->pendings, 300 * HZ) == 0) {
			ret = ERR_PTR(-ETIMEDOUT);
			goto out;
		}
	}
	smp_rmb();
	ret = (void *)ws->private;
out:
	put_wait_station(ws);
	return ret;
}
EXPORT_SYMBOL_GPL(wait_at_station);
