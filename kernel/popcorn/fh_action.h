#ifndef __POPCORN_FAULT_HANDLING_ACTION_H__
#define __POPCORN_FAULT_HANDLING_ACTION_H__

#include <linux/mm.h>

enum {
	FH_ACTION_INVALID = 0x00,
	FH_ACTION_FOLLOW = 0x10,
	FH_ACTION_RETRY = 0x20,
	FH_ACTION_WAIT = 0x01,
	FH_ACTION_LOCAL = 0x02,
	FH_ACTION_DELAY = 0x04,

	PC_FAULT_FLAG_REMOTE = 0x200,

	FH_ACTION_MAX_FOLLOWER = 8,
};

static inline bool fault_for_write(unsigned long flags)
{
	return !!(flags & FAULT_FLAG_WRITE);
}

static inline bool fault_for_read(unsigned long flags)
{
	return !fault_for_write(flags);
}

unsigned short get_fh_action(bool at_remote,
                             unsigned long fh_flags,
                             unsigned fault_flags
                             );
#endif
