#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>

#include "ring_buffer.h"

struct ring_buffer_header {
	bool ready;
	size_t size;
};

static int __init_ring_buffer(struct ring_buffer *rb, const char *fmt, va_list args)
{
	void *buffer;
	buffer = (void *)__get_free_pages(GFP_KERNEL, MAX_ORDER - 1);
	if (!buffer) {
		kfree(rb);
		return -ENOMEM;
	}

	rb->wraparound = 1;
	rb->total_size = 1 << (PAGE_SHIFT + MAX_ORDER - 1);
	rb->buffer_start = buffer;
	rb->buffer_end = buffer + rb->total_size;
	rb->head = rb->tail = rb->buffer_start;

	vsnprintf(rb->name, sizeof(rb->name), fmt, args);
	return 0;
}

int ring_buffer_init(struct ring_buffer *rb, const char *namefmt, ...)
{
	int ret;
	va_list args;

	va_start(args, namefmt);
	ret = __init_ring_buffer(rb, namefmt, args);
	va_end(args);

	return ret;
}

struct ring_buffer *ring_buffer_create(void **addr, const char *namefmt, ...)
{
	struct ring_buffer *rb;
	int ret;
	va_list args;

	rb = kzalloc(sizeof(*rb), GFP_KERNEL);
	if (!rb) return ERR_PTR(ENOMEM);

	va_start(args, namefmt);
	ret = __init_ring_buffer(rb, namefmt, args);
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

void *ring_buffer_get(struct ring_buffer *rb, size_t size)
{
	struct ring_buffer_header *header;
	spin_lock(&rb->lock);
	if (rb->tail + sizeof(*header) + size > rb->buffer_end) {
		/* Put the terminator and wrap around the ring */
		header = rb->tail;
		header->ready = true;
		header->size = rb->buffer_end - (rb->tail + sizeof(*header));
		rb->tail = rb->buffer_start;
		rb->wraparound--;
	}
	if (rb->wraparound == 0) {
		if (rb->tail + sizeof(*header) + size > rb->head) {
			spin_unlock(&rb->lock);
			return NULL;
		}
	}
	header = rb->tail;
	rb->tail += sizeof(*header) + size;
	spin_unlock(&rb->lock);

	header->ready = false;
	header->size = size;

	return header + sizeof(*header);
}

void ring_buffer_put(struct ring_buffer *rb, void *buffer)
{
	struct ring_buffer_header *header;
	header = buffer - sizeof(*header);
	spin_lock(&rb->lock);
	header->ready = true;
	if (header == rb->head) {
		while (header->ready) {
			rb->head += sizeof(*header) + header->size;
			if (rb->head == rb->buffer_end) {
				rb->head = rb->buffer_start;
				rb->wraparound++;
			}
			if (rb->head == rb->tail) break;
		}
	}
	spin_unlock(&rb->lock);
}

MODULE_LICENSE("GPL");
