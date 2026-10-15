/*
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ring.h"

/*
 * Plain loops instead of memcpy: the toolchain's memcpy goes byte by byte.
 * Whole 32-bit words when both sides are aligned (frames of even channel
 * counts in word aligned buffers, the normal case).
 */
static void copy_samples(int16_t *dst, const int16_t *src, uint32_t n)
{
	if ((((uintptr_t)dst | (uintptr_t)src) & 3u) == 0) {
		/* Four words at a time: one LDM/STM pair on the Cortex-M4 */
		struct quad {
			uint32_t w[4];
		};
		struct quad *dq = (struct quad *)dst;
		const struct quad *sq = (const struct quad *)src;
		uint32_t nq = n / 8;
		uint32_t *d = (uint32_t *)&dq[nq];
		const uint32_t *s = (const uint32_t *)&sq[nq];

		for (uint32_t i = 0; i < nq; i++) {
			dq[i] = sq[i];
		}
		for (uint32_t i = 0; i < (n / 2) % 4; i++) {
			d[i] = s[i];
		}
		if (n & 1u) {
			dst[n - 1] = src[n - 1];
		}
	} else {
		for (uint32_t i = 0; i < n; i++) {
			dst[i] = src[i];
		}
	}
}

static inline uint32_t load_acquire(const uint32_t *p)
{
	return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}

static inline void store_release(uint32_t *p, uint32_t v)
{
	__atomic_store_n(p, v, __ATOMIC_RELEASE);
}

void ring_init(struct frame_ring *r, int16_t *buf, uint32_t capacity,
	       uint32_t channels)
{
	r->buf = buf;
	r->capacity = capacity;
	r->channels = channels;
	r->head = 0;
	r->tail = 0;
}

uint32_t ring_fill(const struct frame_ring *r)
{
	uint32_t tail = load_acquire(&r->tail);
	uint32_t head = load_acquire(&r->head);

	return head - tail;
}

uint32_t ring_write(struct frame_ring *r, const int16_t *src, uint32_t frames)
{
	uint32_t head = r->head;
	uint32_t space = r->capacity - (head - load_acquire(&r->tail));
	uint32_t n = frames < space ? frames : space;
	uint32_t idx = head & (r->capacity - 1);
	uint32_t first = r->capacity - idx;

	if (first > n) {
		first = n;
	}

	copy_samples(&r->buf[idx * r->channels], src, first * r->channels);
	copy_samples(r->buf, &src[first * r->channels], (n - first) * r->channels);

	store_release(&r->head, head + n);

	return n;
}

uint32_t ring_read(struct frame_ring *r, int16_t *dst, uint32_t frames)
{
	uint32_t tail = r->tail;
	uint32_t fill = load_acquire(&r->head) - tail;
	uint32_t n = frames < fill ? frames : fill;
	uint32_t idx = tail & (r->capacity - 1);
	uint32_t first = r->capacity - idx;

	if (first > n) {
		first = n;
	}

	copy_samples(dst, &r->buf[idx * r->channels], first * r->channels);
	copy_samples(&dst[first * r->channels], r->buf, (n - first) * r->channels);

	store_release(&r->tail, tail + n);

	return n;
}

uint32_t ring_peek(const struct frame_ring *r, uint32_t frames,
		   struct ring_span *span)
{
	uint32_t tail = r->tail;
	uint32_t fill = load_acquire(&r->head) - tail;
	uint32_t n = frames < fill ? frames : fill;
	uint32_t idx = tail & (r->capacity - 1);
	uint32_t first = r->capacity - idx;

	if (first > n) {
		first = n;
	}
	span->p1 = &r->buf[idx * r->channels];
	span->n1 = first;
	span->p2 = r->buf;
	span->n2 = n - first;

	return n;
}

void ring_consume(struct frame_ring *r, uint32_t frames)
{
	store_release(&r->tail, r->tail + frames);
}

uint32_t ring_drop(struct frame_ring *r, uint32_t frames)
{
	uint32_t tail = r->tail;
	uint32_t fill = load_acquire(&r->head) - tail;
	uint32_t n = frames < fill ? frames : fill;

	store_release(&r->tail, tail + n);

	return n;
}

void ring_flush(struct frame_ring *r)
{
	store_release(&r->tail, load_acquire(&r->head));
}
