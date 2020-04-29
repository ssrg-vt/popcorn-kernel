// SPDX-License-Identifier: GPL-2.0, BSD
#ifndef __INTERNAL_PROCESS_SERVER_H__
#define __INTERNAL_PROCESS_SERVER_H__

#include <popcorn/process_server.h>

enum {
	INDEX_OUTBOUND = 0,
	INDEX_INBOUND = 1,
};

struct task_struct;
struct field_arch;

inline void __lock_remote_contexts(spinlock_t *remote_contexts_lock, int index);
inline void __unlock_remote_contexts(spinlock_t *remote_contexts_lock,
				     int index);

int save_thread_info(struct field_arch *arch);
int restore_thread_info(struct field_arch *arch, bool restore_segments);
#endif /* __INTERNAL_PROCESS_SERVER_H_ */
