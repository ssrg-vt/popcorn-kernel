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

static DEFINE_SPINLOCK(__print_lock);
static char *__print_buffer = NULL;

void print_page_owner(struct page *page, char *tag)
{
	if (!unlikely(__print_buffer)) {
		__print_buffer = kmalloc(PAGE_SIZE, GFP_KERNEL);
	}
	spin_lock(&__print_lock);
	bitmap_print_to_pagebuf(
			true, __print_buffer, page->owners, MAX_POPCORN_NODES);
	printk("page_owner %s: %p %s", tag, page, __print_buffer);
	spin_unlock(&__print_lock);
}

