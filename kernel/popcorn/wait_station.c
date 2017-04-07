/**
 * Waiting stations allows threads to be waited for a given 
 * number of events are completed
 */
#include <linux/kernel.h>

#include "wait_station.h"

#define MAX_WAIT_STATIONS 64

struct wait_station wait_stations[MAX_WAIT_STATIONS];

DEFINE_SPINLOCK(wait_station_lock);
DECLARE_BITMAP(wait_station_available, MAX_WAIT_STATIONS) = { 0 };

struct wait_station *get_wait_station(pid_t pid)
{
	int id;
	struct wait_station *ws;

	spin_lock(&wait_station_lock);
	id = find_first_zero_bit(wait_station_available, MAX_WAIT_STATIONS);
	ws = wait_stations + id;
	set_bit(id, wait_station_available);
	spin_unlock(&wait_station_lock);

	ws->id = id;
	ws->pid = pid;
	ws->private = NULL;
	init_completion(&ws->pendings);
	//printk(" *[%d]: %d allocated\n", pid, id);

	return ws;
}

void put_wait_station(pid_t pid, int id)
{
	spin_lock(&wait_station_lock);
	BUG_ON(!test_bit(id, wait_station_available));
	clear_bit(id, wait_station_available);
	spin_unlock(&wait_station_lock);
	//printk(" *[%d]: %d returned\n", pid, id);
}
