#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ring_buf {
	uint32_t size;
	uint32_t head;
	uint32_t tail;
	uint8_t *buf;
};

static inline void ring_buf_init(struct ring_buf *rb, uint32_t size, uint8_t *buf)
{
	rb->size = size;
	rb->head = 0;
	rb->tail = 0;
	rb->buf = buf;
}

static inline void ring_buf_reset(struct ring_buf *rb)
{
	rb->head = 0;
	rb->tail = 0;
}

static inline uint32_t ring_buf_put(struct ring_buf *rb, const uint8_t *data, uint32_t len)
{
	uint32_t written = 0;
	uint32_t free_space = (rb->head - rb->tail - 1 + rb->size) % rb->size;

	for (uint32_t i = 0; i < len && written < free_space; i++) {
		rb->buf[rb->tail] = data[i];
		rb->tail = (rb->tail + 1) % rb->size;
		written++;
	}

	return written;
}

static inline uint32_t ring_buf_get(struct ring_buf *rb, uint8_t *data, uint32_t len)
{
	uint32_t read = 0;

	while (read < len && rb->head != rb->tail) {
		data[read] = rb->buf[rb->head];
		rb->head = (rb->head + 1) % rb->size;
		read++;
	}

	return read;
}

#ifdef __cplusplus
}
#endif
