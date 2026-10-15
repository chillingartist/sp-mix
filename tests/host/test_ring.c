/*
 * SPDX-License-Identifier: MIT
 */

#include "ring.h"
#include "test.h"

TEST(test_ring_basic)
{
	int16_t mem[16 * 2];
	int16_t in[6] = { 1, 2, 3, 4, 5, 6 };
	int16_t out[6] = { 0 };
	struct frame_ring r;

	ring_init(&r, mem, 16, 2);
	CHECK(ring_fill(&r) == 0);
	CHECK(ring_write(&r, in, 3) == 3);
	CHECK(ring_fill(&r) == 3);
	CHECK(ring_read(&r, out, 2) == 2);
	CHECK(out[0] == 1 && out[3] == 4);
	CHECK(ring_fill(&r) == 1);
	CHECK(ring_drop(&r, 5) == 1);
	CHECK(ring_fill(&r) == 0);
	CHECK(ring_read(&r, out, 2) == 0);
}

TEST(test_ring_wrap_and_full)
{
	int16_t mem[8 * 2];
	int16_t in[2 * 20], out[2 * 20];
	struct frame_ring r;
	int16_t next = 0, expect = 0;

	ring_init(&r, mem, 8, 2);

	/* Many cycles across the wrap point, never losing order */
	for (int cycle = 0; cycle < 50; cycle++) {
		uint32_t n = (uint32_t)(cycle % 7) + 1;
		uint32_t w, rd;

		for (uint32_t i = 0; i < 2 * n; i++) {
			in[i] = next++;
		}
		w = ring_write(&r, in, n);
		next = (int16_t)(next - 2 * (int)(n - w));
		rd = ring_read(&r, out, (uint32_t)(cycle % 5) + 1);
		for (uint32_t i = 0; i < 2 * rd; i++) {
			CHECK(out[i] == expect);
			expect++;
		}
		CHECK(ring_fill(&r) <= 8);
	}

	/* Full ring refuses more */
	ring_flush(&r);
	for (int i = 0; i < 16; i++) {
		in[i] = (int16_t)i;
	}
	CHECK(ring_write(&r, in, 8) == 8);
	CHECK(ring_write(&r, in, 1) == 0);
}

TEST(test_ring_peek_consume)
{
	int16_t mem[8 * 2];
	int16_t in[2 * 8];
	struct frame_ring r;
	struct ring_span s;
	int16_t next = 0, expect = 0;

	ring_init(&r, mem, 8, 2);

	/* Zero-copy reads see the same order as ring_read(), across the wrap */
	for (int cycle = 0; cycle < 60; cycle++) {
		uint32_t n = (uint32_t)(cycle % 7) + 1;
		uint32_t w, got;

		for (uint32_t i = 0; i < 2 * n; i++) {
			in[i] = next++;
		}
		w = ring_write(&r, in, n);
		next = (int16_t)(next - 2 * (int)(n - w));

		got = ring_peek(&r, (uint32_t)(cycle % 5) + 1, &s);
		CHECK(got == s.n1 + s.n2);
		CHECK(s.n2 == 0 || s.p2 == mem);
		/* Peeking does not consume */
		CHECK(ring_fill(&r) >= got);
		for (uint32_t i = 0; i < 2 * s.n1; i++) {
			CHECK(s.p1[i] == expect);
			expect++;
		}
		for (uint32_t i = 0; i < 2 * s.n2; i++) {
			CHECK(s.p2[i] == expect);
			expect++;
		}
		ring_consume(&r, got);
		CHECK(ring_fill(&r) <= 8);
	}

	ring_flush(&r);
	CHECK(ring_peek(&r, 4, &s) == 0 && s.n1 == 0 && s.n2 == 0);
}
