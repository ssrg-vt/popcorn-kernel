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
	size_t size:22;
} __attribute__((packed));

size_t ring_buffer_usage(struct ring_buffer *rb)
{
	size_t used = 0;
	unsigned long flags;

	spin_lock_irqsave(&rb->lock, flags);
	if (rb->head_chunk == rb->tail_chunk) {
		if (!rb->wraparounded) {
			used = rb->tail - rb->head;
		} else {
			used  = RB_CHUNK_SIZE * rb->nr_chunks;
			used -= rb->head - rb->tail;
		}
	} else {
		used  = rb->chunk_end[rb->head_chunk] - rb->head;
		used += rb->tail - rb->chunk_start[rb->tail_chunk];
		used += ((rb->tail_chunk + rb->wraparounded * rb->nr_chunks)
				- rb->head_chunk - 1) * RB_CHUNK_SIZE;
	}
#ifdef CONFIG_POPCORN_STAT
	rb->peak_usage = max(rb->peak_usage, used);
#endif
	spin_unlock_irqrestore(&rb->lock, flags);

	return used;
}

static int __init_ring_buffer(struct ring_buffer *rb, const unsigned short nr_chunks, const char *fmt, va_list args)
{
	unsigned short i;
	unsigned short j;
	int ret = 0;

	for (i = 0; i < nr_chunks; i++) {
		void *buffer = (void *)__get_free_pages(GFP_KERNEL, RB_CHUNK_ORDER);
		if (!buffer) {
			ret = -ENOMEM;
			for (j = 0; j < nr_chunks; j++) {
				if (rb->chunk_start[j]) {
					free_pages((unsigned long)rb->chunk_start[j], RB_CHUNK_ORDER);
					rb->chunk_start[j] = NULL;
				}
			}
			return ret;
		}
		rb->chunk_start[i] = buffer;
		rb->chunk_end[i] = buffer + RB_CHUNK_SIZE;
		rb->dma_addr_base[i] = 0;
	}

	spin_lock_init(&rb->lock);
	rb->head_chunk = rb->tail_chunk = 0;
	rb->nr_chunks = nr_chunks;
	rb->wraparounded = 0;
	rb->head = rb->tail = rb->chunk_start[0];
#ifdef CONFIG_POPCORN_STAT
	rb->total_size = RB_CHUNK_SIZE * nr_chunks;
	rb->peak_usage = 0;
#endif

	vsnprintf(rb->name, sizeof(rb->name), fmt, args);
	return 0;
}

int ring_buffer_init(struct ring_buffer *rb, const char *namefmt, ...)
{
	int ret;
	va_list args;

	va_start(args, namefmt);
	ret = __init_ring_buffer(rb, RB_NR_CHUNKS, namefmt, args);
	va_end(args);

	return ret;
}

struct ring_buffer *ring_buffer_create(const char *namefmt, ...)
{
	struct ring_buffer *rb;
	int ret;
	va_list args;

	rb = kzalloc(sizeof(*rb), GFP_KERNEL);
	if (!rb) return ERR_PTR(ENOMEM);

	va_start(args, namefmt);
	ret = __init_ring_buffer(rb, RB_NR_CHUNKS, namefmt, args);
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
		if (rb->chunk_start[i]) {
			free_pages((unsigned long)rb->chunk_start[i], RB_CHUNK_ORDER);
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

static inline bool __get_next_chunk(struct ring_buffer *rb, unsigned short *index) {
	(*index)++;
	if (*index >= rb->nr_chunks) {
		*index = 0;
		return true;
	}
	return false;
}

void *ring_buffer_get_mapped(struct ring_buffer *rb, size_t size, dma_addr_t *dma_addr)
{
	struct ring_buffer_header *header;
	unsigned long flags;
	int alloc_index;

	size = ALIGN(sizeof(*header) + size, RB_ALIGN) - sizeof(*header);

	spin_lock_irqsave(&rb->lock, flags);
	if (rb->tail + sizeof(*header) + size > rb->chunk_end[rb->tail_chunk]) {
		/* Put the terminator and wrap around the ring */
		header = rb->tail;
		__set_header(header, true,
				rb->chunk_end[rb->tail_chunk] - (rb->tail + sizeof(*header)));
		if (__get_next_chunk(rb, &rb->tail_chunk))
			rb->wraparounded++;
		rb->tail = rb->chunk_start[rb->tail_chunk];
	}

	/* Is buffer full? */
	if (rb->wraparounded && rb->head_chunk == rb->tail_chunk) {
		if (rb->tail + sizeof(*header) + size > rb->head) {
			spin_unlock_irqrestore(&rb->lock, flags);
			return NULL;
		}
	}

	alloc_index = rb->tail_chunk;
	header = rb->tail;
	rb->tail += sizeof(*header) + size;
	if (rb->tail + ALIGN(sizeof(*header), RB_ALIGN) >=
				rb->chunk_end[rb->tail_chunk]) {
		/* Skip small trailor */
		size += rb->chunk_end[rb->tail_chunk] - rb->tail;
		if (__get_next_chunk(rb, &rb->tail_chunk))
			rb->wraparounded++;
		rb->tail = rb->chunk_start[rb->tail_chunk];
	}
	__set_header(header, false, size);
	spin_unlock_irqrestore(&rb->lock, flags);
#ifdef CONFIG_POPCORN_CHECK_SANITY
	memset(header + 1, 0xcd, size);
#endif

	if (dma_addr) {
		*dma_addr = rb->dma_addr_base[alloc_index] +
			((void *)(header + 1) - rb->chunk_start[alloc_index]);
	}
	return header + 1;
}

void *ring_buffer_get(struct ring_buffer *rb, size_t size)
{
	return ring_buffer_get_mapped(rb, size, NULL);
}

void ring_buffer_put(struct ring_buffer *rb, void *buffer)
{
	struct ring_buffer_header *header;
	unsigned long flags;

	header = buffer - sizeof(*header);
#ifdef CONFIG_POPCORN_CHECK_SANITY
	memset(buffer, 0xaf, header->size);	/* put poision */
#endif

	spin_lock_irqsave(&rb->lock, flags);
	header->reclaim = true;

	header = rb->head;
	while (header->reclaim) {
#ifdef CONFIG_POPCORN_CHECK_SANITY
		BUG_ON(header->magic != RB_HEADER_MAGIC);
#endif
		rb->head += sizeof(*header) + header->size;
		if (rb->head == rb->chunk_end[rb->head_chunk]) {
			if (__get_next_chunk(rb, &rb->head_chunk))
				rb->wraparounded--;
			rb->head = rb->chunk_start[rb->head_chunk];
		}
		if (rb->head == rb->tail) break;
		header = rb->head;
	}
	spin_unlock_irqrestore(&rb->lock, flags);
}
