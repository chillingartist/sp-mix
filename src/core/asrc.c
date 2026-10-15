/*
 * SPDX-License-Identifier: MIT
 */

#include <math.h>
#include <stddef.h>

#include "asrc.h"

_Static_assert(ASRC_TAPS % 4 == 0, "asrc_pull handles four taps per round");

#define PI_F 3.14159265359f

/* Kaiser window shape and passband edge (fraction of Nyquist) */
#ifndef KAISER_BETA
#define KAISER_BETA	9.1f
#endif
#ifndef CUTOFF
#define CUTOFF		0.97f
#endif

static float bessel_i0(float x)
{
	float sum = 1.0f, term = 1.0f, q = x * x / 4.0f;

	for (int k = 1; k < 30; k++) {
		term *= q / (float)(k * k);
		sum += term;
		if (term < 1e-9f * sum) {
			break;
		}
	}

	return sum;
}

static float kernel(float t)
{
	/* t in input samples, window support [-ASRC_HALF, ASRC_HALF] */
	float r = t / (float)ASRC_HALF;
	float w, s;

	if (r <= -1.0f || r >= 1.0f) {
		return 0.0f;
	}

	w = bessel_i0(KAISER_BETA * sqrtf(1.0f - r * r)) / bessel_i0(KAISER_BETA);
	if (fabsf(t) < 1e-6f) {
		s = CUTOFF;
	} else {
		s = sinf(PI_F * CUTOFF * t) / (PI_F * t);
	}

	return s * w;
}

/* Normalised taps for read position i + frac */
static void phase_taps(float frac, float c[ASRC_TAPS])
{
	float sum = 0.0f;

	/* Tap j multiplies x[i + j - ASRC_PRE] */
	for (int j = 0; j < ASRC_TAPS; j++) {
		c[j] = kernel(frac - (float)(j - ASRC_PRE));
		sum += c[j];
	}
	for (int j = 0; j < ASRC_TAPS; j++) {
		c[j] /= sum;
	}
}

void asrc_init(struct asrc *a)
{
	float c0[ASRC_TAPS], c1[ASRC_TAPS];

	phase_taps(0.0f, c0);
	for (int p = 0; p < ASRC_PHASES; p++) {
		phase_taps((float)(p + 1) / (float)ASRC_PHASES, c1);
		for (int j = 0; j < ASRC_TAPS; j++) {
			a->coef[p][2 * j] = c0[j];
			a->coef[p][2 * j + 1] = c1[j] - c0[j];
			c0[j] = c1[j];
		}
	}

	asrc_reset(a);
}

/* Plain loops: the toolchain's memcpy/memmove go byte by byte */
static void copy_floats(float *dst, const float *src, uint32_t n)
{
	for (uint32_t i = 0; i < n; i++) {
		dst[i] = src[i];
	}
}

void asrc_reset(struct asrc *a)
{
	for (uint32_t i = 0; i < 2 * ASRC_BUF_FRAMES; i++) {
		a->buf[i] = 0.0f;
	}
	a->n = ASRC_PRE;
	a->pos = (uint64_t)ASRC_PRE << 32;
}

uint64_t asrc_step_from_ppm(float ppm)
{
	/* 2^32 / 1e6. Through int32_t: one VCVT instead of the library's
	 * float to int64 conversion (double arithmetic, ~170 cycles); exact
	 * for |ppm| < 5e5, the rate loop clamps to 1000.
	 */
	float d = sp1m_clampf(ppm, -400000.0f, 400000.0f) * 4294.967296f;

	return ASRC_ONE + (uint64_t)(int64_t)(int32_t)d;
}

uint32_t asrc_frames_needed(const struct asrc *a, uint32_t n_out, uint64_t step)
{
	uint64_t last;
	uint32_t want;

	if (n_out == 0) {
		return 0;
	}

	last = a->pos + step * (n_out - 1);
	want = (uint32_t)(last >> 32) + ASRC_HALF + 1;

	return want > a->n ? want - a->n : 0;
}

uint32_t asrc_push(struct asrc *a, const float *in, uint32_t frames)
{
	uint32_t room = ASRC_BUF_FRAMES - a->n;
	uint32_t n = frames < room ? frames : room;

	copy_floats(&a->buf[2 * a->n], in, 2 * n);
	a->n += n;

	return n;
}

float *asrc_in_buf(struct asrc *a, uint32_t frames, uint32_t *fit)
{
	uint32_t room = ASRC_BUF_FRAMES - a->n;

	*fit = frames < room ? frames : room;

	return &a->buf[2 * a->n];
}

void asrc_in_commit(struct asrc *a, uint32_t frames)
{
	uint32_t room = ASRC_BUF_FRAMES - a->n;

	a->n += frames < room ? frames : room;
}

/*
 * Eight consecutive floats into registers. On the Cortex-M4F one VLDM takes
 * 1 + 8 cycles (ARM DDI0439 table 7-1) where separate VLDRs cost 2 each
 * unless the compiler happens to place them back to back; the registers are
 * fixed so that the compiler cannot split the load.
 */
#if defined(__ARM_ARCH_7EM__) && defined(__ARM_FP) && !defined(SP1M_HOST)
#define ASRC_LOAD8(p, a0, a1, a2, a3, a4, a5, a6, a7, regs)			\
	__asm__("vldmia %[ptr]!, {" regs "}"					\
		: [ptr] "+r"(p), "=t"(a0), "=t"(a1), "=t"(a2), "=t"(a3),	\
		  "=t"(a4), "=t"(a5), "=t"(a6), "=t"(a7)			\
		: "m"(*(const float (*)[8])(p)))
#define ASRC_REG(name, r) register float name __asm__(r)
#else
#define ASRC_LOAD8(p, a0, a1, a2, a3, a4, a5, a6, a7, regs)			\
	do {									\
		a0 = (p)[0]; a1 = (p)[1]; a2 = (p)[2]; a3 = (p)[3];		\
		a4 = (p)[4]; a5 = (p)[5]; a6 = (p)[6]; a7 = (p)[7];		\
		(p) += 8;							\
	} while (0)
#define ASRC_REG(name, r) float name
#endif

/*
 * Four taps: interpolate their coefficients at @mu, multiply with the stereo
 * history. Accumulation order as before: l0/r0 take taps 0 and 2, l1/r1
 * taps 1 and 3. FIRST starts the sums instead of adding to 0.0f (without
 * -ffast-math the compiler keeps those additions).
 */
#define ASRC_ROUND(FIRST)							\
	do {									\
		ASRC_REG(c0, "s16"); ASRC_REG(d0, "s17");			\
		ASRC_REG(c1, "s18"); ASRC_REG(d1, "s19");			\
		ASRC_REG(c2, "s20"); ASRC_REG(d2, "s21");			\
		ASRC_REG(c3, "s22"); ASRC_REG(d3, "s23");			\
		ASRC_REG(x0, "s24"); ASRC_REG(x1, "s25");			\
		ASRC_REG(x2, "s26"); ASRC_REG(x3, "s27");			\
		ASRC_REG(x4, "s28"); ASRC_REG(x5, "s29");			\
		ASRC_REG(x6, "s30"); ASRC_REG(x7, "s31");			\
										\
		ASRC_LOAD8(c, c0, d0, c1, d1, c2, d2, c3, d3, "s16-s23");	\
		ASRC_LOAD8(x, x0, x1, x2, x3, x4, x5, x6, x7, "s24-s31");	\
		float ca = c0 + mu * d0;					\
		float cb = c1 + mu * d1;					\
		float cc = c2 + mu * d2;					\
		float cd = c3 + mu * d3;					\
		if (FIRST) {							\
			l0 = ca * x0;						\
			r0 = ca * x1;						\
			l1 = cb * x2;						\
			r1 = cb * x3;						\
		} else {							\
			l0 += ca * x0;						\
			r0 += ca * x1;						\
			l1 += cb * x2;						\
			r1 += cb * x3;						\
		}								\
		l0 += cc * x4;							\
		r0 += cc * x5;							\
		l1 += cd * x6;							\
		r1 += cd * x7;							\
	} while (0)

/* Full scale float to a right aligned, sign extended 24-bit sample */
static inline int32_t f_to_i24(float v)
{
#if defined(__ARM_ARCH_7EM__) && defined(__ARM_FP) && !defined(SP1M_HOST)
	int32_t q;

	/* Q31 with saturation at +-1.0 (VCVT saturates), then >> 8 */
	__asm__("vcvt.s32.f32 %0, %0, #31" : "+t"(v));
	__asm__("vmov %0, %1" : "=r"(q) : "t"(v));
	return q >> 8;
#else
	int32_t q;

	if (v >= 1.0f) {
		q = INT32_MAX;
	} else if (v <= -1.0f) {
		q = INT32_MIN;
	} else {
		q = (int32_t)(v * 2147483648.0f);
	}
	return q >> 8;
#endif
}

static inline __attribute__((always_inline)) void pull(struct asrc *a, float *out_f, int32_t *out_i,
			uint32_t n_out, uint64_t step)
{
	uint64_t pos = a->pos;
	uint32_t drop;

	for (uint32_t k = 0; k < n_out; k++) {
		uint32_t i = (uint32_t)(pos >> 32);
		uint32_t frac = (uint32_t)pos;
		uint32_t ph = frac >> (32 - ASRC_PHASE_BITS);
		float mu = (float)((frac >> (16 - ASRC_PHASE_BITS)) & 0xffff) *
			   (1.0f / 65536.0f);
		const float *c = a->coef[ph];
		float l0, r0, l1, r1;

		if (i + ASRC_HALF >= a->n) {
			/* Starved: emit silence rather than read stale data. */
			if (out_f) {
				out_f[2 * k] = 0.0f;
				out_f[2 * k + 1] = 0.0f;
			} else {
				out_i[2 * k] = 0;
				out_i[2 * k + 1] = 0;
			}
			pos += step;
			continue;
		}

		const float *x = &a->buf[2 * (i - ASRC_PRE)];

		ASRC_ROUND(1);
#pragma GCC unroll 8
		for (int j = 4; j < ASRC_TAPS; j += 4) {
			ASRC_ROUND(0);
		}

		if (out_f) {
			out_f[2 * k] = l0 + l1;
			out_f[2 * k + 1] = r0 + r1;
		} else {
			out_i[2 * k] = f_to_i24(l0 + l1);
			out_i[2 * k + 1] = f_to_i24(r0 + r1);
		}
		pos += step;
	}

	/* Keep ASRC_PRE frames of history before the new read position. */
	drop = (uint32_t)(pos >> 32);
	drop = drop > ASRC_PRE ? drop - ASRC_PRE : 0;
	if (drop > a->n) {
		drop = a->n;
	}
	/* Forward copy: the destination lies before the source */
	copy_floats(a->buf, &a->buf[2 * drop], 2 * (a->n - drop));
	a->n -= drop;
	a->pos = pos - ((uint64_t)drop << 32);
}

void asrc_pull(struct asrc *a, float *out, uint32_t n_out, uint64_t step)
{
	pull(a, out, NULL, n_out, step);
}

void asrc_pull_i24(struct asrc *a, int32_t *out, uint32_t n_out, uint64_t step)
{
	pull(a, NULL, out, n_out, step);
}
