/*
 * SPDX-License-Identifier: MIT
 *
 * Single-producer single-consumer ring of interleaved int16 audio frames.
 * The USB receive path writes, the audio thread reads. Indices are free
 * running frame counters; capacity must be a power of two.
 */

#ifndef SP1M_RING_H_
#define SP1M_RING_H_

#include "sp1m.h"

struct frame_ring {
	int16_t *buf;
	uint32_t capacity;
	uint32_t channels;
	uint32_t head;	/* frames written, owned by the producer */
	uint32_t tail;	/* frames read, owned by the consumer */
};

void ring_init(struct frame_ring *r, int16_t *buf, uint32_t capacity,
	       uint32_t channels);

/* Frames available to the consumer. Safe from either side. */
uint32_t ring_fill(const struct frame_ring *r);

/* Producer: copy up to @frames frames in; returns frames written. */
uint32_t ring_write(struct frame_ring *r, const int16_t *src, uint32_t frames);

/* Consumer: copy up to @frames frames out; returns frames read. */
uint32_t ring_read(struct frame_ring *r, int16_t *dst, uint32_t frames);

/* Up to two contiguous runs of buffered frames (the second after the wrap) */
struct ring_span {
	const int16_t *p1;
	uint32_t n1;
	const int16_t *p2;
	uint32_t n2;
};

/*
 * Consumer, zero-copy: up to @frames frames in place; returns their number.
 * They stay owned by the consumer until ring_consume() releases them.
 */
uint32_t ring_peek(const struct frame_ring *r, uint32_t frames,
		   struct ring_span *span);
void ring_consume(struct frame_ring *r, uint32_t frames);

/* Consumer: discard up to @frames frames; returns frames discarded. */
uint32_t ring_drop(struct frame_ring *r, uint32_t frames);

/* Consumer: discard everything currently buffered. */
void ring_flush(struct frame_ring *r);

#endif /* SP1M_RING_H_ */
