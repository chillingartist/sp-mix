/*
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

#include "mixer.h"
#include "test.h"

static void unity_params(struct mix_params *p)
{
	memset(p, 0, sizeof(*p));
	for (int t = 0; t < SP1M_TRACKS; t++) {
		p->track_gain[t] = 1.0f;
	}
	p->master_gain = 1.0f;
}

TEST(test_mixer_balance_law)
{
	CHECK_NEAR(mixer_balance_gain(0.0f, false), 1.0, 1e-6);
	CHECK_NEAR(mixer_balance_gain(0.0f, true), 1.0, 1e-6);
	CHECK_NEAR(mixer_balance_gain(1.0f, false), 0.0, 1e-6);
	CHECK_NEAR(mixer_balance_gain(1.0f, true), 1.0, 1e-6);
	CHECK_NEAR(mixer_balance_gain(-1.0f, true), 0.0, 1e-6);
	CHECK_NEAR(mixer_balance_gain(-0.5f, true), 0.70710678, 1e-5);
	CHECK_NEAR(mixer_balance_gain(-0.5f, false), 1.0, 1e-6);
	/* Out of range is clamped */
	CHECK_NEAR(mixer_balance_gain(3.0f, false), 0.0, 1e-6);
}

TEST(test_mixer_mute_solo)
{
	struct mix_params p;
	float gl[SP1M_TRACKS], gr[SP1M_TRACKS];

	unity_params(&p);
	p.mute_mask = 0x2;
	mixer_target_gains(&p, gl, gr);
	CHECK(gl[0] == 1.0f && gl[1] == 0.0f && gr[1] == 0.0f && gl[2] == 1.0f);

	/* Solo overrides mute and silences the rest */
	p.solo_mask = 0x2 | 0x8;
	mixer_target_gains(&p, gl, gr);
	CHECK(gl[0] == 0.0f && gl[1] == 1.0f && gl[2] == 0.0f && gl[3] == 1.0f);
}

TEST(test_mixer_process)
{
	static int16_t in[SP1M_IN_CH * 64];
	static float out[2 * 64];
	struct mixer m;
	struct mix_params p;
	float opk;

	unity_params(&p);
	p.master_gain = 0.5f;
	for (int i = 0; i < 64; i++) {
		/* Track 1 left only, track 3 right only; alternating sign
		 * (fs/2) passes the DC blocker unchanged
		 */
		int16_t s = (i & 1) ? -1 : 1;

		in[i * SP1M_IN_CH + 0] = (int16_t)(8192 * s);
		in[i * SP1M_IN_CH + 5] = (int16_t)(-4096 * s);
	}

	mixer_init(&m, 48000.0f, 0.005f);
	/* Starts from silence and converges within a few time constants */
	for (int blk = 0; blk < 40; blk++) {
		mixer_process(&m, &p, in, out, 64);
	}
	CHECK_NEAR(out[2 * 63], -0.25 * 0.5, 1e-4);
	CHECK_NEAR(out[2 * 63 + 1], 0.125 * 0.5, 1e-4);

	opk = mixer_take_peak(&m);
	CHECK_NEAR(opk, 0.125, 1e-3);
	CHECK(mixer_take_peak(&m) == 0.0f);

	/* A gain step never jumps within one block */
	p.track_gain[0] = 0.0f;
	mixer_process(&m, &p, in, out, 64);
	for (int i = 1; i < 64; i++) {
		CHECK(fabsf(fabsf(out[2 * i]) - fabsf(out[2 * (i - 1)])) < 0.01f);
	}
	CHECK(fabsf(out[2 * 63]) < fabsf(out[0]));
}

TEST(test_mixer_dc_and_clip)
{
	static int16_t in[SP1M_IN_CH * 64];
	static float out[2 * 64];
	struct mixer m;
	struct mix_params p;
	float peak = 0.0f;
	uint32_t n = 0;

	unity_params(&p);
	mixer_init(&m, 48000.0f, 0.005f);

	/* A DC offset from the host decays away */
	for (int i = 0; i < 64; i++) {
		in[i * SP1M_IN_CH + 0] = 8192;
		in[i * SP1M_IN_CH + 1] = -8192;
	}
	for (int blk = 0; blk < 750; blk++) {
		mixer_process(&m, &p, in, out, 64);
	}
	CHECK(fabsf(out[2 * 63]) < 1e-3f && fabsf(out[2 * 63 + 1]) < 1e-3f);

	/* Overdriven bass: the clipper comes after the blocker, so nothing
	 * goes past full scale (the other order gave +1.5 dB at 30 Hz)
	 */
	memset(in, 0, sizeof(in));
	mixer_silence(&m);
	for (int blk = 0; blk < 750; blk++) {
		for (int i = 0; i < 64; i++, n++) {
			float v = sinf(2.0f * 3.14159265f * 30.0f * (float)n / 48000.0f);
			int16_t x = (int16_t)(32767.0f * v);

			for (int ch = 0; ch < SP1M_IN_CH; ch++) {
				in[i * SP1M_IN_CH + ch] = x;
			}
		}
		mixer_process(&m, &p, in, out, 64);
		for (int i = 0; i < 2 * 64; i++) {
			peak = fmaxf(peak, fabsf(out[i]));
		}
	}
	CHECK(peak <= 1.0f);
	CHECK(peak > 0.99f);
	CHECK(m.clipped > 0);
}

TEST(test_mixer_dc_block)
{
	static float buf[2 * 4800];
	struct dc_block d;
	double peak = 0.0;

	dc_block_init(&d, 48000.0f, 5.0f);

	/* Full-scale DC decays away within a second */
	for (int b = 0; b < 10; b++) {
		for (int i = 0; i < 4800; i++) {
			buf[2 * i] = 1.0f;
			buf[2 * i + 1] = -1.0f;
		}
		dc_block_process(&d, buf, 4800);
	}
	CHECK(fabsf(buf[2 * 4799]) < 1e-3f && fabsf(buf[2 * 4799 + 1]) < 1e-3f);

	/* 100 Hz passes with less than 0.02 dB loss */
	dc_block_reset(&d);
	for (int b = 0; b < 10; b++) {
		for (int i = 0; i < 4800; i++) {
			float v = sinf(2.0f * 3.14159265f * 100.0f * (float)(b * 4800 + i) / 48000.0f);

			buf[2 * i] = v;
			buf[2 * i + 1] = v;
		}
		dc_block_process(&d, buf, 4800);
	}
	for (int i = 0; i < 4800; i++) {
		peak = fmax(peak, fabs(buf[2 * i]));
	}
	CHECK(peak > 0.9977 && peak < 1.0005);
}

TEST(test_mixer_soft_clip)
{
	float prev = 0.0f;

	CHECK(mixer_soft_clip(0.5f) == 0.5f);
	CHECK(mixer_soft_clip(-0.85f) == -0.85f);
	for (float x = 0.0f; x < 8.0f; x += 0.01f) {
		float y = mixer_soft_clip(x);

		CHECK(y >= prev - 1e-6f);
		CHECK(y <= 1.0f);
		CHECK(mixer_soft_clip(-x) == -y);
		prev = y;
	}
	/* Continuous at the knee */
	CHECK_NEAR(mixer_soft_clip(MIXER_CLIP_KNEE + 1e-4f), MIXER_CLIP_KNEE, 2e-4);
}
