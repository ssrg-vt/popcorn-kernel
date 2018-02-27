#ifndef __POPCORN_RING_BUFFER_H__
#define __POPCORN_RING_BUFFER_H__

struct ring_buffer {
	void *head;
	void *tail;
	int wraparounded;

	spinlock_t lock;
	void *buffer_start;
	void *buffer_end;
	unsigned int nr_chunks;
	size_t total_size;
#ifdef CONFIG_POPCORN_STAT
	size_t peak_usage;
#endif
	char name[80];
};

struct ring_buffer *ring_buffer_create(size_t size, int (*map)(unsigned long, size_t), const char *namefmt, ...);
int ring_buffer_init(struct ring_buffer *rb, size_t size, int (*map)(unsigned long, size_t), const char *namefmt, ...);
void *ring_buffer_get(struct ring_buffer *rb, size_t size);
void ring_buffer_ready(struct ring_buffer *rb, void *buffer);
void ring_buffer_put(struct ring_buffer *rb, void *buffer);
void ring_buffer_destroy(struct ring_buffer *rb);

size_t ring_buffer_usage(struct ring_buffer *rb);
#endif
