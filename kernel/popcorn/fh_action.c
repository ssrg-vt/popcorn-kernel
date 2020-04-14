#include <linux/kernel.h>
#include <linux/seq_file.h>

#include "fh_action.h"

/*
 * Current fault handling type
 *   (L/R) For local or remote
 *   (-/i) Ownership revocation pending
 *   (R/W) For read or write
 *
 * Current fault type
 *   (L/R) For local or remote
 *   (R/W) For read or write
 *
 * e.g., at origin, L-WRW means the page is currently locked for handling
 * local "fault for write" and "request to handle" remote fault for write.
 * In this case, just retry immediately.
 */

static unsigned long __fh_action_stat[64] = { 0 };
static const unsigned short fh_action_table[64] = {
    // L - R, LR (at origin)
	FH_ACTION_FOLLOW,
    // L - R, RR
	FH_ACTION_RETRY | FH_ACTION_WAIT | FH_ACTION_LOCAL,
    // L - R, LW
	FH_ACTION_RETRY | FH_ACTION_WAIT,
    // L - R, RW
    FH_ACTION_RETRY,
    // L - W, LR
	FH_ACTION_FOLLOW,
    // L - W, RR
    FH_ACTION_RETRY,
    // L - W, LW
	FH_ACTION_FOLLOW,
    // L - W, RW
	FH_ACTION_RETRY,
    // L i R, LR
	FH_ACTION_INVALID,
    // L i R, RR (at origin)
	FH_ACTION_INVALID,
    // L i R, LW
	FH_ACTION_INVALID,
    // L i R, RW
	FH_ACTION_INVALID,
    // L i W, LR
	FH_ACTION_INVALID,
    // L i W, RR
	FH_ACTION_INVALID,
    // L i W, LW
	FH_ACTION_INVALID,
    // L i W, RW
	FH_ACTION_INVALID,

    // R - R, LR
	FH_ACTION_FOLLOW,
    // R - R, RR
	FH_ACTION_RETRY | FH_ACTION_WAIT | FH_ACTION_LOCAL,
    // R - R, LW
	FH_ACTION_RETRY | FH_ACTION_WAIT,
    // R - R, RW
	FH_ACTION_RETRY | FH_ACTION_WAIT,
    // R - W, LR
	FH_ACTION_RETRY | FH_ACTION_WAIT,
    // R - W, RR
	FH_ACTION_RETRY,
    // R - W, LW
	FH_ACTION_RETRY | FH_ACTION_WAIT,
    // R - W, RW
	FH_ACTION_RETRY,
    // R i R, LR
	FH_ACTION_INVALID,
    // R i R, RR (at origin)
    FH_ACTION_INVALID,
    // R i R, LW
	FH_ACTION_INVALID,
    // R i R, RW
	FH_ACTION_INVALID,
    // R i W, LR
	FH_ACTION_INVALID,
    // R i W, RR
	FH_ACTION_INVALID,
    // R i W, LW
	FH_ACTION_INVALID,
    // R i W, RW
	FH_ACTION_INVALID,

    /*
     * At remote
	 *      - *i*R* are impossible since the origin never asks remotes for
     *        pages while an ownership revocation is pended.
	 *      - R**R* are impossible since the origin never asks a page twice.
	 *      - Ri*** are impossible henceforth.
	 */

    // L - R, LR
	FH_ACTION_FOLLOW,
    // L - R, RR, implies remote doesn't own this page
	FH_ACTION_INVALID,
    // L - R, LW
	FH_ACTION_RETRY  | FH_ACTION_WAIT,
    // L - R, RW
	FH_ACTION_INVALID,
    // L - W, LR
	FH_ACTION_FOLLOW,
    // L - W, RR
	FH_ACTION_RETRY  | FH_ACTION_WAIT | FH_ACTION_LOCAL | FH_ACTION_DELAY,
    // L - W, LW
	FH_ACTION_FOLLOW,
    // L - W, RW
	FH_ACTION_RETRY  | FH_ACTION_WAIT | FH_ACTION_LOCAL | FH_ACTION_DELAY,
    // L i R, LR, no waiter should exist when finishing ownership revocation
	FH_ACTION_FOLLOW | FH_ACTION_RETRY | FH_ACTION_DELAY,
    // L i R, RR
	FH_ACTION_INVALID,
    // L i R, LW
	FH_ACTION_RETRY  | FH_ACTION_WAIT | FH_ACTION_DELAY,
    // L i R, RW
	FH_ACTION_INVALID,
    // L i W, LR (same to LiRLR)
	FH_ACTION_FOLLOW | FH_ACTION_RETRY | FH_ACTION_DELAY,
    // L i W, RR
	FH_ACTION_INVALID,
    // L i W, LW (same to LiRLR)
	FH_ACTION_FOLLOW | FH_ACTION_RETRY | FH_ACTION_DELAY,
    // L i W, RW
	FH_ACTION_INVALID,
    // R - R, LR
	FH_ACTION_INVALID,
    // R - R, RR
	FH_ACTION_INVALID,
    // R - R, LW
	FH_ACTION_RETRY | FH_ACTION_WAIT,
    // R - R, RW
	FH_ACTION_INVALID,
    // R - W, LR
	FH_ACTION_RETRY | FH_ACTION_WAIT,
    // R - W, RR
	FH_ACTION_INVALID,
    // R - W, LW
	FH_ACTION_RETRY | FH_ACTION_WAIT,
    // R - W, RW
    FH_ACTION_INVALID,
    // R i R, LR
	FH_ACTION_INVALID,
    // R i R, RR
	FH_ACTION_INVALID,
    // R i R, LW
	FH_ACTION_INVALID,
    // R i R, RW
    FH_ACTION_INVALID,
    // R i W, LR
	FH_ACTION_INVALID,
    // R i W, RR
	FH_ACTION_INVALID,
    // R i W, LW
	FH_ACTION_INVALID,
    // R i W, RW
	FH_ACTION_INVALID,
};

unsigned short get_fh_action(bool at_remote,
                             unsigned long fh_flags,
                             unsigned fault_flags)
{
	unsigned short i;
	i  = (at_remote << 5);
	i |= (fh_flags & 0x07) << 2;
	i |= !!(fault_for_write(fault_flags)) << 1;
	i |= !!(fault_flags & PC_FAULT_FLAG_REMOTE) << 0;

	return fh_action_table[i];
}

void fh_action_stat(struct seq_file *seq, void *v)
{
	int i;
	for (i = 0; i < ARRAY_SIZE(__fh_action_stat) / 4; i++) {
		if (seq) {
			seq_printf(seq,
                       "%2d %-12lu %2d %-12lu %2d %-12lu %2d %-12lu\n",
				       i,
					   __fh_action_stat[i], i + 16,
                       __fh_action_stat[i + 16], i + 32,
                       __fh_action_stat[i + 32], i + 48,
                       __fh_action_stat[i + 48]);
		} else {
			__fh_action_stat[i] = 0;
			__fh_action_stat[i + 16] = 0;
			__fh_action_stat[i + 32] = 0;
			__fh_action_stat[i + 48] = 0;
		}
	}
}
