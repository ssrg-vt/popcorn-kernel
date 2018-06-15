/**
 * @file cpuinfo.c
 *
 * Popcorn Linux PPC64 cpuinfo implementation
 *
 * @author: Sang-Hoon Kim <sanghoon@vt.edu>
 */
#include <linux/kernel.h>
#include <linux/errno.h>

#include <popcorn/cpuinfo.h>

int fill_cpu_info(struct remote_cpu_info *res)
{
	return -EINVAL;
}
