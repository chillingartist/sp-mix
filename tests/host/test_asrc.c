/*
 * SPDX-License-Identifier: MIT
 */

#include <math.h>
#include <string.h>

#include "asrc.h"
#include "test.h"

static struct asrc a;

TEST(test_asrc_accounting)
{
	float in[2 * 128], out[2 * 64];
	const float ppm = 500.0f;
	uint64_t step = asrc_step_from_ppm(ppm);
	uint64_t consumed = 0;
	const int blocks = 20000;

	memset(in, 0, sizeof(in));
	asrc_init(&a);

	for (int b = 0; b <= blocks; b++) {
		uint32_t n = asrc_frames_needed(&a, 64, step);

		CHECK(n <= 128);
		CHECK(asrc_push(&a, in, n) == n);
		asrc_pull(&a, out, 64, step);
		/* The first block also fills the filter history */
		if (b > 0) {
			consumed += n;
		}
	}

	/* Input consumed per output frame equals the requested ratio */
	CHECK_NEAR((double)consumed / (64.0 * blocks), 1.0 + ppm * 1e-6, 2e-6);
	CHECK(asrc_frames_needed(&a, 0, step) == 0);
}

/* Error of resampling a sine against the exact sine, in dB re. amplitude. */
static double sine_error_db(double freq, float ppm)
{
	const double fs = 48000.0;
	const double amp = 0.5;
	uint64_t step = asrc_step_from_ppm(ppm);
	double ratio = (double)step / 4294967296.0;
	float in[2 * 160], out[2 * 64];
	uint64_t in_idx = 0;
	double err2 = 0.0, n_err = 0.0;
	double t_out = 0.0;	/* output time in input samples */

	asrc_init(&a);
	/* asrc_reset pre-fills ASRC_PRE frames of silence: input sample k
	 * sits at buffer index k + ASRC_PRE and the read position starts at
	 * ASRC_PRE, so output j reads input time j * ratio.
	 */
	for (int b = 0; b < 3000; b++) {
		uint32_t n = asrc_frames_needed(&a, 64, step);

		for (uint32_t i = 0; i < n; i++) {
			double v = amp * sin(2.0 * M_PI * freq * (double)in_idx / fs);

			in[2 * i] = (float)v;
			in[2 * i + 1] = (float)-v;
			in_idx++;
		}
		asrc_push(&a, in, n);
		asrc_pull(&a, out, 64, step);

		for (int k = 0; k < 64; k++) {
			double ref = amp * sin(2.0 * M_PI * freq * t_out / fs);

			if (b > 10) {
				double e = out[2 * k] - ref;

				err2 += e * e;
				n_err += 1.0;
				CHECK(fabs(out[2 * k] + out[2 * k + 1]) < 1e-6);
			}
			t_out += ratio;
		}
	}

	return 10.0 * log10(err2 / n_err / (amp * amp / 2.0));
}

TEST(test_asrc_quality)
{
	double e1k = sine_error_db(1000.0, 300.0f);
	double e5k = sine_error_db(5000.0, -700.0f);
	double e10k = sine_error_db(10000.0, 300.0f);
	double e16k = sine_error_db(16000.0, 300.0f);

	printf("  asrc error: 1k %.1f dB, 5k %.1f dB, 10k %.1f dB, 16k %.1f dB\n",
	       e1k, e5k, e10k, e16k);
	CHECK(e1k < -95.0);
	CHECK(e5k < -90.0);
	CHECK(e10k < -85.0);
	CHECK(e16k < -80.0);
}

static int32_t ref_i24(float v)
{
	/* VCVT.S32.F32 #31 semantics: scale, round toward zero, saturate */
	double q = (double)v * 2147483648.0;
	int32_t i;

	if (q >= 2147483647.0) {
		i = INT32_MAX;
	} else if (q <= -2147483648.0) {
		i = INT32_MIN;
	} else {
		i = (int32_t)q;
	}
	return i >> 8;
}

/* The I2S output path gives exactly the float path's samples, converted */
TEST(test_asrc_i24)
{
	static struct asrc b;
	float in[2 * 160], out[2 * 64];
	int32_t out_i[2 * 64];
	uint64_t step = asrc_step_from_ppm(-420.0f);
	uint32_t idx = 0, mism = 0, clipped = 0;

	asrc_init(&a);
	asrc_init(&b);
	for (int blk = 0; blk < 400; blk++) {
		uint32_t n = asrc_frames_needed(&a, 64, step);
		uint32_t fit;
		float *dst;

		CHECK(n == asrc_frames_needed(&b, 64, step));
		for (uint32_t i = 0; i < n; i++, idx++) {
			/* Up to 1.3x full scale: the saturation is exercised */
			float v = 1.3f * sinf(2.0f * 3.14159265f * 997.0f * (float)idx / 48000.0f);

			in[2 * i] = v;
			in[2 * i + 1] = -0.5f * v + 1e-4f * (float)(idx % 7);
		}
		CHECK(asrc_push(&a, in, n) == n);
		dst = asrc_in_buf(&b, n, &fit);
		CHECK(fit == n);
		memcpy(dst, in, sizeof(float) * 2 * n);
		asrc_in_commit(&b, n);

		asrc_pull(&a, out, 64, step);
		asrc_pull_i24(&b, out_i, 64, step);
		for (int k = 0; k < 2 * 64; k++) {
			int32_t e = ref_i24(out[k]);

			if (e != out_i[k]) {
				mism++;
			}
			if (out_i[k] == 0x7fffff || out_i[k] == -0x800000) {
				clipped++;
			}
			CHECK(out_i[k] <= 0x7fffff && out_i[k] >= -0x800000);
		}
	}
	CHECK(mism == 0);
	CHECK(clipped > 0);
}
