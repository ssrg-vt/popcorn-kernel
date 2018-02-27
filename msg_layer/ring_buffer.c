#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>

#include "ring_buffer.h"

#ifdef CONFIG_POPCORN_CHECK_SANITY
#define RB_HEADER_MAGIC 0xa9
#endif
#define RB_ALIGN 64
#define RB_NR_CHUNKS 8

struct ring_buffer_header {
	bool reclaim:1;
#ifdef CONFIG_POPCORN_CHECK_SANITY
	unsigned int magic:8;
#endif
	size_t size:23;
} __attribute__((packed, aligned(64)));

size_t ring_buffer_usage(struct ring_buffer *rb)
{
	size_t used = 0;

	if (rb->head_chunk == rb->tail_chunk) {
		if (!rb->wraparounded) {
			used = rb->tail - rb->head;
		} else {
			used  = RB_CHUNK_SIZE * rb->nr_chunks;
			used -= rb->head - rb->tail;
		}
	} else {
		used  = rb->buffer_end[rb->head_chunk] - rb->head;
		used += rb->tail - rb->buffer_start[rb->tail_chunk];
		used += ((rb->tail_chunk + rb->wraparounded * rb->nr_chunks)
				- rb->head_chunk - 1) * RB_CHUNK_SIZE;
	}
	return used;
}

static int __init_ring_buffer(struct ring_buffer *rb, unsigned int nr_chunks, int (*map)(unsigned long, size_t), const char *fmt, va_list args)
{
	int i;
	int ret = 0;

	for (i = 0; i < nr_chunks; i++) {
		void *buffer = (void *)__get_free_pages(GFP_KERNEL, RB_CHUNK_ORDER);
		if (!buffer) {
			ret = -ENOMEM;
			goto out_free;
		}
		if (map) map((unsigned long)buffer, RB_CHUNK_SIZE);
		rb->buffer_start[i] = buffer;
		rb->buffer_end[i] = buffer + RB_CHUNK_SIZE;
	}

	rb->head_chunk = rb->tail_chunk = 0;
	rb->nr_chunks = nr_chunks;
	rb->wraparounded = 0;
	rb->head = rb->tail = rb->buffer_start[0];
#ifdef CONFIG_POPCORN_STAT
	rb->total_size = RB_CHUNK_SIZE * nr_chunks;
	rb->peak_usage = 0;
#endif

	vsnprintf(rb->name, sizeof(rb->name), fmt, args);
	return 0;

out_free:
	for (i = 0; i < nr_chunks; i++) {
		if (rb->buffer_start[i]) {
			free_pages((unsigned long)rb->buffer_start[i], RB_CHUNK_ORDER);
			rb->buffer_start[i] = NULL;
		}
	}
	return ret;
}

int ring_buffer_init(struct ring_buffer *rb, int (*map)(unsigned long, size_t), const char *namefmt, ...)
{
	int ret;
	va_list args;

	va_start(args, namefmt);
	ret = __init_ring_buffer(rb, RB_NR_CHUNKS, map, namefmt, args);
	va_end(args);

	return ret;
}

struct ring_buffer *ring_buffer_create(int (*map)(unsigned long, size_t), const char *namefmt, ...)
{
	struct ring_buffer *rb;
	int ret;
	va_list args;

	rb = kzalloc(sizeof(*rb), GFP_KERNEL);
	if (!rb) return ERR_PTR(ENOMEM);

	va_start(args, namefmt);
	ret = __init_ring_buffer(rb, RB_NR_CHUNKS, map, namefmt, args);
	va_end(args);

	if (ret) {
		kfree(rb);
		return ERR_PTR(ENOMEM);
	}
	return rb;
}


void ring_buffer_destroy(struct ring_buffer *rb)
{
	int i;
	for (i = 0; i < RB_MAX_CHUNKS; i++) {
		if (rb->buffer_start[i]) {
			free_pages((unsigned long)rb->buffer_start[i], RB_CHUNK_ORDER);
		}
	}
}

static inline void __set_header(struct ring_buffer_header *header, bool reclaim, size_t size) {
	header->reclaim = reclaim;
	header->size = size;
#ifdef CONFIG_POPCORN_CHECK_SANITY
	header->magic = RB_HEADER_MAGIC;
#endif
}

static inline bool __get_next_chunk(struct ring_buffer *rb, unsigned int *index) {
	(*index)++;
	if (*index >= rb->nr_chunks) {
		*index = 0;
#ifdef CONFIG_POPCORN_STAT
		rb->peak_usage = max(rb->peak_usage, ring_buffer_usage(rb));
#endif
		return true;
	}
	return false;
}

void *ring_buffer_get(struct ring_buffer *rb, size_t size)
{
	struct ring_buffer_header *header;
	unsigned long flags;

	size = ALIGN(sizeof(*header) + size, RB_ALIGN) - sizeof(*header);

	spin_lock_irqsave(&rb->lock, flags);
	if (rb->tail + sizeof(*header) + size > rb->buffer_end[rb->tail_chunk]) {
		/* Put the terminator and wrap around the ring */
		header = rb->tail;
		__set_header(header, true,
				rb->buffer_end[rb->tail_chunk] - (rb->tail + sizeof(*header)));
		if (__get_next_chunk(rb, &rb->tail_chunk))
			rb->wraparounded++;
		rb->tail = rb->buffer_start[rb->tail_chunk];
	}

	/* Is buffer full? */
	if (rb->wraparounded && rb->head_chunk == rb->tail_chunk) {
		if (rb->tail + sizeof(*header) + size > rb->head) {
			spin_unlock(&rb->lock);
			return NULL;
		}
	}

	header = rb->tail;
	rb->tail += sizeof(*header) + size;
	if (rb->tail + sizeof(*header) >= rb->buffer_end[rb->tail_chunk]) {
		/* Skip small trailor */
		size += rb->buffer_end[rb->tail_chunk] - rb->tail;
		if (__get_next_chunk(rb, &rb->tail_chunk))
			rb->wraparounded++;
		rb->tail = rb->buffer_start[rb->tail_chunk];
	}
	__set_header(header, false, size);
	spin_unlock_irqrestore(&rb->lock, flags);

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
			BUG_ON(header->magic != RB_HEADER_MAGIC);
#endif
			rb->head += sizeof(*header) + header->size;
			if (rb->head == rb->buffer_end[rb->head_chunk]) {
				if (__get_next_chunk(rb, &rb->head_chunk))
					rb->wraparounded--;
				rb->head = rb->buffer_start[rb->head_chunk];
			}
			if (rb->head == rb->tail) break;
			header = rb->head;
		}
	}
	spin_unlock_irqrestore(&rb->lock, flags);
}
