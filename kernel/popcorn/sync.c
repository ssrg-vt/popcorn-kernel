/*
 * sync.c
 * Copyright (C) 2018 Ho-Ren(Jack) Chuang <horenc@vt.edu>
 *
 * Distributed under terms of the MIT license.
 */
#include <popcorn/debug.h>
#include <linux/syscalls.h>

// TODO: take ip into consideration
unsigned long long system_tso_wr_cnt = 0;
spinlock_t tso_lock;

void collect_tso_wr(void)
{
	if (current->accu_tso_wr_cnt) {
		spin_lock(&tso_lock);
		system_tso_wr_cnt += current->accu_tso_wr_cnt;
		spin_unlock(&tso_lock);
		printk("[%d]: exit contributs #%llu %llu -> system %llu\n",
								current->pid, current->tso_region_cnt,
								current->accu_tso_wr_cnt,
								system_tso_wr_cnt);
	}
}

void clean_tso_wr(void)
{
	spin_lock(&tso_lock);
	system_tso_wr_cnt = 0;
	spin_unlock(&tso_lock);
}

#ifdef CONFIG_POPCORN
SYSCALL_DEFINE2(popcorn_tso_begin, int, a, void __user *, b)
{
//	if (current->tso_region || current->tso_wr_cnt || current->tso_wx_cnt)
//		PCNPRINTK_ERR("BUG tso_region order violation when \"lock\"\n");
//
//	current->tso_region = true;

	return 0;
}

SYSCALL_DEFINE2(popcorn_tso_fence, int, a, void __user *, b)
{
//	if (!current->tso_region)
//		PCNPRINTK_ERR("BUG tso_region order violation when \"unlock\"\n");
//	if (!current->tso_wr_cnt)
//			//|| !current->tso_wx_cnt)
//		PCNPRINTK_ERR("WARNNING no benefits here\n");
//
//	current->tso_region_cnt++;
//	//printk("[%d] %s(): #%llu tso_wr %llu/%llu\n", current->pid, __func__,
//	//					current->tso_region_cnt, current->tso_wr_cnt,
//	//					current->accu_tso_wr_cnt);
//
//	current->tso_wr_cnt = 0;
//	current->tso_wx_cnt = 0;
	return 0;
}

SYSCALL_DEFINE2(popcorn_tso_end, int, a, void __user *, b)
{
//	current->tso_region = false;
	return 0;
}

SYSCALL_DEFINE2(popcorn_tso_begin_manual, int, a, void __user *, b)
{
	if (current->tso_region || current->tso_wr_cnt || current->tso_wx_cnt)
		PCNPRINTK_ERR("BUG tso_region order violation when \"lock\"\n");

	current->tso_region = true;

	return 0;
}

SYSCALL_DEFINE2(popcorn_tso_fence_manual, int, a, void __user *, b)
{
	if (!current->tso_region)
		PCNPRINTK_ERR("BUG tso_region order violation when \"unlock\"\n");
	if (!current->tso_wr_cnt)
			//|| !current->tso_wx_cnt)
		PCNPRINTK_ERR("WARNNING no benefits here\n");

	current->tso_region_cnt++;
	//printk("[%d] %s(): #%llu tso_wr %llu/%llu\n", current->pid, __func__,
	//					current->tso_region_cnt, current->tso_wr_cnt,
	//					current->accu_tso_wr_cnt);

	current->tso_wr_cnt = 0;
	current->tso_wx_cnt = 0;
	return 0;
}

SYSCALL_DEFINE2(popcorn_tso_end_manual, int, a, void __user *, b)
{
	current->tso_region = false;
	return 0;
}
#else // CONFIG_POPCORN
SYSCALL_DEFINE2(popcorn_tso_begin, int, a void __user *, b)
{
	PCNPRINTK_ERR("Kernel is not configured to use popcorn\n");
	return -EPERM;
}

SYSCALL_DEFINE2(popcorn_tso_fence, int, a, void __user *, b)
{
	PCNPRINTK_ERR("Kernel is not configured to use popcorn\n");
	return -EPERM;
}

SYSCALL_DEFINE2(popcorn_tso_end, int, a, void __user *, b)
{
	PCNPRINTK_ERR("Kernel is not configured to use popcorn\n");
	return -EPERM;
}


SYSCALL_DEFINE2(popcorn_tso_begin_manual, int, a void __user *, b)
{
	PCNPRINTK_ERR("Kernel is not configured to use popcorn\n");
	return -EPERM;
}

SYSCALL_DEFINE2(popcorn_tso_fence_manual, int, a, void __user *, b)
{
	PCNPRINTK_ERR("Kernel is not configured to use popcorn\n");
	return -EPERM;
}

SYSCALL_DEFINE2(popcorn_tso_end_manual, int, a, void __user *, b)
{
	PCNPRINTK_ERR("Kernel is not configured to use popcorn\n");
	return -EPERM;
}
#endif

int __init popcorn_sync_init(void)
{
	spin_lock_init(&tso_lock);
	return 0;
}
