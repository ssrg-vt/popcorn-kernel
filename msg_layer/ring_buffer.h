#ifndef __POPCORN_RING_BUFFER_H__
#define __POPCORN_RING_BUFFER_H__

/* send/recv pool */
/* 16*8 doesn't work*/
/* 16*6 doesn't work on SC on fox4 (Cavium) */
#define MSG_POOL_SIZE (16 * 5)
#define RB_MAX_CHUNKS	128 /* Max. Actual used size  RB_NR_CHUNKS */
#define RB_NR_CHUNKS	128 /* Actual used size */
#define RB_CHUNK_ORDER	(MAX_ORDER - 1)
#define RB_CHUNK_SIZE	(PAGE_SIZE << RB_CHUNK_ORDER)

struct ring_buffer {
	unsigned short head_chunk;
	void *head;
	unsigned short tail_chunk;
	void *tail;
	int wraparounded;

	spinlock_t lock;
	void *chunk_start[RB_MAX_CHUNKS];
	void *chunk_end[RB_MAX_CHUNKS];
	dma_addr_t dma_addr_base[RB_MAX_CHUNKS];
	unsigned int nr_chunks;

#ifdef CONFIG_POPCORN_STAT
	size_t total_size;
	size_t peak_usage;
#endif
	char name[80];
};

struct ring_buffer *ring_buffer_create(const char *namefmt, ...);
int ring_buffer_init(struct ring_buffer *rb, const char *namefmt, ...);
void *ring_buffer_get(struct ring_buffer *rb, size_t size);
void *ring_buffer_get_mapped(struct ring_buffer *rb, size_t size, dma_addr_t *dma_addr);
void ring_buffer_ready(struct ring_buffer *rb, void *buffer);
void ring_buffer_put(struct ring_buffer *rb, void *buffer);
void ring_buffer_destroy(struct ring_buffer *rb);

size_t ring_buffer_usage(struct ring_buffer *rb);
#endif
