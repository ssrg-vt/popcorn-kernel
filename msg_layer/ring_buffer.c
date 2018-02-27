#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>

#include "ring_buffer.h"

#ifdef CONFIG_POPCORN_CHECK_SANITY
#define RING_BUFFER_MAGIC 0xa9
#endif

struct ring_buffer_header {
	bool reclaim:1;
#ifdef CONFIG_POPCORN_CHECK_SANITY
	unsigned int magic:8;
#endif
	size_t size:23;
} __attribute__((packed, aligned (64)));

size_t ring_buffer_usage(struct ring_buffer *rb)
{
	if (rb->wraparounded) {
		return rb->buffer_end - rb->head + rb->tail - rb->buffer_start;
	}
	return rb->tail - rb->head;
}

static int __init_ring_buffer(struct ring_buffer *rb, size_t size, int(*map)(unsigned long, size_t), const char *fmt, va_list args)
{
	void *buffer;

	buffer = (void *)__get_free_pages(GFP_KERNEL, MAX_ORDER - 1);
	if (!buffer) {
		kfree(rb);
		return -ENOMEM;
	}

	rb->wraparounded = 0;
	rb->total_size = PAGE_SIZE << (MAX_ORDER - 1);
	rb->buffer_start = buffer;
	rb->buffer_end = buffer + rb->total_size;
	rb->head = rb->tail = rb->buffer_start;
#ifdef CONFIG_POPCORN_STAT
	rb->peak_usage = 0;
#endif

	vsnprintf(rb->name, sizeof(rb->name), fmt, args);
	return 0;
}

int ring_buffer_init(struct ring_buffer *rb, size_t size, int (*map)(unsigned long, size_t), const char *namefmt, ...)
{
	int ret;
	va_list args;

	va_start(args, namefmt);
	ret = __init_ring_buffer(rb, size, map, namefmt, args);
	va_end(args);

	return ret;
}

struct ring_buffer *ring_buffer_create(size_t size, int (*map)(unsigned long, size_t), const char *namefmt, ...)
{
	struct ring_buffer *rb;
	int ret;
	va_list args;

	rb = kzalloc(sizeof(*rb), GFP_KERNEL);
	if (!rb) return ERR_PTR(ENOMEM);

	va_start(args, namefmt);
	ret = __init_ring_buffer(rb, size, map, namefmt, args);
	va_end(args);

	if (ret) {
		kfree(rb);
		return ERR_PTR(ENOMEM);
	}
	return rb;
}


void ring_buffer_destroy(struct ring_buffer *rb)
{
	if (rb->buffer_start) {
		free_pages((unsigned long)rb->buffer_start, MAX_ORDER - 1);
	}
}

static inline void __set_header(struct ring_buffer_header *header, bool reclaim, size_t size)
{
	header->reclaim = reclaim;
	header->size = size;
#ifdef CONFIG_POPCORN_CHECK_SANITY
	header->magic = RING_BUFFER_MAGIC;
#endif
}

void *ring_buffer_get(struct ring_buffer *rb, size_t size)
{
	struct ring_buffer_header *header;
	unsigned long flags;

	spin_lock_irqsave(&rb->lock, flags);
	if (rb->tail + sizeof(*header) + size > rb->buffer_end) {
		/* Put the terminator and wrap around the ring */
		header = rb->tail;
		__set_header(header, true, rb->buffer_end - (rb->tail + sizeof(*header)));
		rb->tail = rb->buffer_start;
		rb->wraparounded++;
	}

	/* Is buffer full? */
	if (rb->wraparounded) {
		if (rb->tail + sizeof(*header) + size > rb->head) {
			spin_unlock(&rb->lock);
			return NULL;
		}
	}

	header = rb->tail;
	rb->tail += sizeof(*header) + size;
	if (rb->tail + sizeof(*header) >= rb->buffer_end) {
		/* Skip small trailor */
		size += rb->buffer_end - rb->tail;
		rb->tail = rb->buffer_start;
		rb->wraparounded++;
	}
	__set_header(header, false, size);
	spin_unlock_irqrestore(&rb->lock, flags);

#ifdef CONFIG_POPCORN_STAT
	rb->peak_usage = max(rb->peak_usage, ring_buffer_usage(rb));
#endif
	return header + 1;
}

void ring_buffer_put(struct ring_buffer *rb, void *buffer)
{
	struct ring_buffer_header *header;
	unsigned long flags;

	header = buffer - sizeof(*header);

	spin_lock_irqsave(&rb->lock, flags);
	header->reclaim = true;
	if (header == rb->head) {
		while (header->reclaim) {
#ifdef CONFIG_POPCORN_CHECK_SANITY
			BUG_ON(header->magic != RING_BUFFER_MAGIC);
#endif
			rb->head += sizeof(*header) + header->size;
			if (rb->head == rb->buffer_end) {
				rb->head = rb->buffer_start;
				rb->wraparounded--;
			}
			if (rb->head == rb->tail) break;
			header = rb->head;
		}
	}
	spin_unlock_irqrestore(&rb->lock, flags);
}
