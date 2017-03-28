#include <linux/mm.h>
#include <linux/slab.h>

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
	printk("\n");
}

void print_page_signature(unsigned char *addr)
{
	unsigned char *p = addr;
	int i, j;
	for (i = 0; i < PAGE_SIZE / 128; i++) {
		unsigned char signature = 0;
		for (j = 0; j < 32; j++) {
			signature = (signature + *p++) & 0xff;
		}
		printk("%02x", signature);
	}
	printk("\n");
}

static DEFINE_SPINLOCK(__print_lock);
static char *__print_buffer = NULL;

void print_page_owner(struct page *page, unsigned long addr, char *tag)
{
	if (!unlikely(__print_buffer)) {
		__print_buffer = kmalloc(PAGE_SIZE, GFP_KERNEL);
	}
	spin_lock(&__print_lock);
	bitmap_print_to_pagebuf(
			true, __print_buffer, page->owners, MAX_POPCORN_NODES);
	printk("page_owner %s: %lx %s", tag, addr, __print_buffer);
	spin_unlock(&__print_lock);
}

