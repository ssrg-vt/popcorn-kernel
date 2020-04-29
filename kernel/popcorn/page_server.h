// SPDX-License-Identifier: GPL-2.0, BSD
#ifndef __INTERNAL_PAGE_SERVER_H__
#define __INTERNAL_PAGE_SERVER_H__

#include <popcorn/page_server.h>

/*
 * Flush pages in remote to the origin
 */
int page_server_flush_remote_pages(struct remote_context *rc);

void free_remote_context_pages(struct remote_context *rc);
int process_madvise_release_from_remote(int from_nid, unsigned long start,
					unsigned long end);

#endif /* __INTERNAL_PAGE_SERVER_H_ */
