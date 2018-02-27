#ifndef __POPCORN_RING_BUFFER_H__
#define __POPCORN_RING_BUFFER_H__

#define RB_MAX_CHUNKS	16
#define RB_CHUNK_ORDER	(MAX_ORDER - 1)
#define RB_CHUNK_SIZE	(PAGE_SIZE << RB_CHUNK_ORDER)

struct ring_buffer {
	unsigned int head_chunk;
	void *head;
	unsigned int tail_chunk;
	void *tail;
	int wraparounded;

	spinlock_t lock;
	void *chunk_start[RB_MAX_CHUNKS];
	void *chunk_end[RB_MAX_CHUNKS];
	unsigned int nr_chunks;

#ifdef CONFIG_POPCORN_STAT
	size_t total_size;
	size_t peak_usage;
#endif
	char name[80];
};

struct ring_buffer *ring_buffer_create(int (*map)(unsigned long, size_t), const char *namefmt, ...);
int ring_buffer_init(struct ring_buffer *rb, int (*map)(unsigned long, size_t), const char *namefmt, ...);
void *ring_buffer_get(struct ring_buffer *rb, size_t size);
void ring_buffer_ready(struct ring_buffer *rb, void *buffer);
void ring_buffer_put(struct ring_buffer *rb, void *buffer);
void ring_buffer_destroy(struct ring_buffer *rb);

size_t ring_buffer_usage(struct ring_buffer *rb);
#endif
