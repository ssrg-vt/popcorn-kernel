/**
 * @file include/popcorn/mvx.h
 *
 * MVX related configuration and functions.
 *
 * @author Xiaoguang Wang, SSRG Virginia Tech, 2019-2020
 */

#ifndef __INCLUDE_POPCORN_MVX_H__
#define __INCLUDE_POPCORN_MVX_H__

#include <linux/sched.h>
#include <popcorn/debug.h>

/* MVX master/follower node id. */
#define MASTER_NID			1		/* ARM64 node as master */
#define FOLLOWER_NID		0		/* x86_64 node as follower */

/* MVX follower process UID/GID. */
#define MVX_FOLLOWER_UID	1000
#define MVX_FOLLOWER_GID	1000

/* MVX Virtual Descriptor Table size. */
#define VDT_SIZE	512

/* VFD types: Read FD, simulated FD. Used in `mvx_message_t.flag`. */
enum mvx_vfd_type {
	MVX_EMPTY = 0,		// empty entry
	MVX_REAL = 1,		// real fd
	MVX_SIM = 2,		// simulated fd
//	MVX_SIM_CLOSE = 3,	// simulated, need close
				// e.g., epoll_create1, socket
	MVX_VFD_MAX			// Maximum number of the VFD state
};

enum mvx_fd_operate {
	MVX_FD_INC = 0,		// increase fd tab
	MVX_FD_DEC = 1,		// decrease fd tab
};

/* The Virtual Descriptor Table and the Index (next available fd). */
extern int fd_vtab[VDT_SIZE];
extern int vtab_count;

/* Stopping MVX. Used in `mvx_message_t.flag`. */
#define MVX_OFF			1
#define MVX_OFF_OFFSET	7

extern int64_t mvx_args[6];
extern const char* dir_whitelist[];
extern size_t whitelist_len;

extern long int mvx_index;

/* 16 bytes */
struct epoll_event_arm64 {
	uint32_t events;
	uint64_t data;
};

/* 12 bytes */
struct epoll_event_x86 {
	uint32_t events;
	uint64_t data;
} __attribute__ ((__packed__));

#define MVX_WARN_ON(condition) {\
	if(unlikely(condition)) \
		pr_err("[MVX Violation] %s:%d %s\n", __FILE__, __LINE__, __func__); \
}

static inline void stop_mvx_process(struct task_struct *tsk)
{
	tsk->is_mvx_process = 0;
	MVXPRINTK("Stop MVX variant ...\n");
}

static inline bool mvx_process(struct task_struct *tsk)
{
	return tsk->is_mvx_process;
}

static inline bool mvx_follower(struct task_struct *tsk)
{
	return tsk->is_follower;
}

static inline int mvx_is_real_fd(int fd)
{
	return fd_vtab[fd] == MVX_REAL;
}

static inline void mvx_print(struct task_struct *tsk)
{
	printk("%s: mvx [%d] %s. mvx proc %d, mvx follower %d\n",
	       __func__, tsk->pid, tsk->comm,
	       mvx_process(tsk), mvx_follower(tsk));
}

#if 0
static inline void mvx_print_fd_vtab(void)
{
	int i;

	MVXPRINTK("%s:\n", __func__);
	for (i = 0; i < VDT_SIZE; i++) {
		if (fd_vtab[i]) {
			MVXPRINTK("  VDT[%2d]: %d\n", i, fd_vtab[i]);
		}
	}
}
#else
static inline void mvx_print_fd_vtab(void) {}
#endif

/**
 * Update fd_vtab entry.
 * */
static inline int mvx_update_fd_vtab(int fd_id, int type, int inc_dec)
{
	if (fd_id < 0 || fd_id >= VDT_SIZE) {
		pr_err("%s: Should not happen for fd id %d\n", __func__, fd_id);
		return -1;	// TODO: fd_id could be very large.
	}

	if (fd_vtab[fd_id] && (inc_dec == MVX_FD_INC)) {
		pr_info("%s: Should not use existed vtab entry %d\n",
			__func__, fd_id);
	}

	fd_vtab[fd_id] = type;	// type=0: empty; type=1: real; type=2: simulated

	if (inc_dec == MVX_FD_INC)
		vtab_count++;
	else
		vtab_count--;

	MVXPRINTK("%s: fd: %d, vtab count %d. type %d, vtab %s.\n", __func__,
		  fd_id, vtab_count, type, (inc_dec == MVX_FD_INC)?"inc":"dec");

	return vtab_count;
}

int mvx_follower_wait_exec(struct task_struct *tsk, unsigned int dst_nid,
			   int syscall, int64_t args[], void *retval,
			   int retsz);
int mvx_follower_post_syscall(struct task_struct *tsk, unsigned int dst_nid,
			   int syscall, int64_t args[], int *retval);

int mvx_master_sync(struct task_struct *tsk, unsigned int dst_nid,
		    int syscall, int64_t args[], int64_t retval);
int mvx_server_start_mvx(struct task_struct *tsk, unsigned int dst_nid);

int mvx_send_reply(long retval, long syscall, int dst_nid);

#endif /* __INCLUDE_POPCORN_MVX_H__ */
