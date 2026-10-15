/*
 * SPDX-License-Identifier: MIT
 *
 * Sequence lock for handing a small struct between threads without blocking
 * either side. The reader never spins: if a write is in progress it keeps
 * its previous copy (the writer may be a lower priority thread that the
 * reader has preempted).
 */

#ifndef SP1M_SEQBOX_H_
#define SP1M_SEQBOX_H_

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static inline void seqbox_write(uint32_t *seq, void *dst, const void *src,
				size_t len)
{
	uint32_t s = __atomic_load_n(seq, __ATOMIC_RELAXED);

	__atomic_store_n(seq, s + 1, __ATOMIC_RELAXED);
	__atomic_thread_fence(__ATOMIC_RELEASE);
	memcpy(dst, src, len);
	__atomic_thread_fence(__ATOMIC_RELEASE);
	__atomic_store_n(seq, s + 2, __ATOMIC_RELAXED);
}

/* Returns false (and leaves @dst unspecified) if a write interfered. */
static inline bool seqbox_read(const uint32_t *seq, void *dst, const void *src,
			       size_t len)
{
	uint32_t s1 = __atomic_load_n(seq, __ATOMIC_ACQUIRE);

	if (s1 & 1u) {
		return false;
	}
	memcpy(dst, src, len);
	__atomic_thread_fence(__ATOMIC_ACQUIRE);

	return __atomic_load_n(seq, __ATOMIC_RELAXED) == s1;
}

#endif /* SP1M_SEQBOX_H_ */
