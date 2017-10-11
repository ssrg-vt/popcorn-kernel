#ifndef __POPCORN_KERNEL_DEBUG_H__
#define __POPCORN_KERNEL_DEBUG_H__
struct page;

void print_page_data(unsigned char *addr);
void print_page_signature(unsigned char *addr);
void print_page_signature_pid(pid_t pid, unsigned char *addr);
void print_page_owner(struct page *page, unsigned long addr, pid_t pid);

int get_file_path(struct file *file, char *sz, size_t size);

void trace_task_status(void);
#endif
