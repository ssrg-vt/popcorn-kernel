// SPDX-License-Identifier: GPL-2.0, BSD
#ifndef __POPCORN_KERNEL_UTIL_H__
#define __POPCORN_KERNEL_UTIL_H__
struct page;

void print_page_data(unsigned char *addr);
void print_page_signature(unsigned char *addr);
void print_page_signature_pid(pid_t pid, unsigned char *addr);
void print_page_owner(unsigned long addr, unsigned long *owners, pid_t pid);

int get_file_path(struct file *file, char *sz, size_t size);

void trace_task_status(void);
#endif
