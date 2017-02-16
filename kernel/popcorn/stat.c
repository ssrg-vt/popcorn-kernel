#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/sched.h>

#include <popcorn/types.h>
#include <popcorn/debug.h>

#ifdef MIGRATION_PROFILE
ktime_t migration_start;
ktime_t migration_end;
#endif

int page_fault_mio = 0;
int fetch = 0;
int local_fetch = 0;
int write = 0;
int concurrent_write = 0;
int most_written_page = 0;
int read = 0;
int invalid = 0;
int ack = 0;
int answer_request = 0;
int answer_request_void = 0;
int request_data = 0;
int pages_allocated = 0;
int most_long_write = 0;
int most_long_read = 0;
int compressed_page_sent = 0;
int not_compressed_page = 0;
int not_compressed_diff_page = 0;

//asmlinkage
long sys_take_time(int start)
{
	if (start == 1)
		trace_printk("s\n");
	else
		trace_printk("e\n");
	return 0;
}

void print_popcorn_stat(void)
{
	PSPRINTK("%s: page_fault %i fetch %i local_fetch %i write %i read %i"
		"most_long_read %i invalid %i ack %i answer_request %i"
		"answer_request_void %i request_data %i most_written_page %i"
		"concurrent_writes %i most long write %i pages_allocated %i"
		"compressed_page_sent %i not_compressed_page %i"
		"not_compressed_diff_page %i (id %d, cpu %d)\n", __func__ ,
		page_fault_mio, fetch, local_fetch, write, read,
		most_long_read, invalid, ack, answer_request,
		answer_request_void, request_data, most_written_page,
		concurrent_write, most_long_write, pages_allocated,
		compressed_page_sent, not_compressed_page,
		not_compressed_diff_page,
		current->origin_nid, current->origin_pid);
}
