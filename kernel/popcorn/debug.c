#include <linux/mm.h>
#include <popcorn/debug.h>

void print_page_data(unsigned char *addr)
{
	int i;
	for (i = 0; i < PAGE_SIZE; i++) {
		if (i % 16 == 0) {
			printk(KERN_INFO"%08lx:", (unsigned long)(addr + i));
		}
		if (i % 4 == 0) {
			printk(" ");
		}
		printk("%02x", *(addr + i));
	}
}
