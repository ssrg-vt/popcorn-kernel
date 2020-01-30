/**
 * @file cpuinfo_popcorn.c
 *
 * Popcorn Linux RISCV64 cpuinfo implementation
 *
 * @author Cesar Philippidis, RASEC Technologies 2020
 */

#include <linux/kernel.h>
#include <linux/cpu.h>
#include <linux/delay.h>
#include <linux/elf.h>
#include <linux/personality.h>

#include <popcorn/cpuinfo.h>
#include <popcorn/pcn_kmsg.h>

int fill_cpu_info(struct remote_cpu_info *res)
{
	return 0;
}
