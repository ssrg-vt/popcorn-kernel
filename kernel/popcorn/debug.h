#ifndef __POPCORN_KERNEL_DEBUG_H__
#define __POPCORN_KERNEL_DEBUG_H__
struct page;

void print_page_data(unsigned char *addr);
void print_page_owner(struct page *page, char *tag);
#endif
