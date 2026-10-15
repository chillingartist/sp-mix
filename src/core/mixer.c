/*
 * SPDX-License-Identifier: MIT
 */

#include <math.h>

#include "mixer.h"

#define HALF_PI 1.57079632679f

void mixer_init(struct mixer *m, float fs, float tau_s)
{
	m->fs = fs;
	m->tau_s = tau_s;
	dc_block_init(&m->dc, fs, MIXER_DC_HZ);
	mixer_silence(m);
	m->out_peak = 0.0f;
	m->clipped = 0;
}

void mixer_silence(struct mixer *m)
{
	for (int t = 0; t < SP1M_TRACKS; t++) {
		m->gl[t] = 0.0f;
		m->gr[t] = 0.0f;
	}
	m->master = 0.0f;
	dc_block_reset(&m->dc);
}

float mixer_balance_gain(float balance, bool right)
{
	float b = sp1m_clampf(balance, -1.0f, 1.0f);

	/* The side the balance moves towards stays at 0 dB. */
	if (right ? (b >= 0.0f) : (b <= 0.0f)) {
		return 1.0f;
	}

	return cosf(fabsf(b) * HALF_PI);
}

void mixer_target_gains(const struct mix_params *p, float gl[SP1M_TRACKS],
			float gr[SP1M_TRACKS])
{
	for (int t = 0; t < SP1M_TRACKS; t++) {
		bool audible;
		float g;

		if (p->solo_mask != 0) {
			audible = (p->solo_mask & (1u << t)) != 0;
		} else {
			audible = (p->mute_mask & (1u << t)) == 0;
		}

		g = audible ? sp1m_clampf(p->track_gain[t], 0.0f, 1.0f) : 0.0f;
		gl[t] = g * mixer_balance_gain(p->balance[t], false);
		gr[t] = g * mixer_balance_gain(p->balance[t], true);
	}
}

/* tanh(u) for u >= 0, Pade approximant, exact 1 from u = 3 on */
static inline float tanh_pos(float u)
{
	if (u >= 3.0f) {
		return 1.0f;
	}

	float u2 = u * u;

	return u * (27.0f + u2) / (27.0f + 9.0f * u2);
}

float mixer_soft_clip(float x)
{
	float a = fabsf(x);

	if (a <= MIXER_CLIP_KNEE) {
		return x;
	}

	const float room = 1.0f - MIXER_CLIP_KNEE;
	float y = MIXER_CLIP_KNEE + room * tanh_pos((a - MIXER_CLIP_KNEE) / room);

	return x < 0.0f ? -y : y;
}

void mixer_process(struct mixer *m, const struct mix_params *p,
		   const int16_t *in, float *out, uint32_t frames)
{
	float tl[SP1M_TRACKS], tr[SP1M_TRACKS];
	float a[2 * SP1M_TRACKS], d[2 * SP1M_TRACKS];
	float alpha, inv_n, m0, m1, opk = m->out_peak;
	uint32_t clipped = 0;

	if (frames == 0) {
		return;
	}

	alpha = 1.0f - expf(-(float)frames / (m->tau_s * m->fs));
	inv_n = 1.0f / (float)frames;

	/*
	 * Track gains and the master move towards their targets by @alpha
	 * per block. Within the block, their product (with the int16 to +-1.0
	 * scale) ramps linearly: a[] at the start, d[] per frame.
	 */
	mixer_target_gains(p, tl, tr);
	m0 = m->master;
	m1 = m0 + (p->master_gain - m0) * alpha;
	for (int t = 0; t < SP1M_TRACKS; t++) {
		float l1 = m->gl[t] + (tl[t] - m->gl[t]) * alpha;
		float r1 = m->gr[t] + (tr[t] - m->gr[t]) * alpha;
		float al = m->gl[t] * m0 * (1.0f / 32768.0f);
		float ar = m->gr[t] * m0 * (1.0f / 32768.0f);

		a[2 * t] = al;
		a[2 * t + 1] = ar;
		d[2 * t] = (l1 * m1 * (1.0f / 32768.0f) - al) * inv_n;
		d[2 * t + 1] = (r1 * m1 * (1.0f / 32768.0f) - ar) * inv_n;
		m->gl[t] = l1;
		m->gr[t] = r1;
	}
	m->master = m1;

	/* Scalars so that the gains stay in FPU registers */
	float a0 = a[0], a1 = a[1], a2 = a[2], a3 = a[3];
	float a4 = a[4], a5 = a[5], a6 = a[6], a7 = a[7];
	const float d0 = d[0], d1 = d[1], d2 = d[2], d3 = d[3];
	const float d4 = d[4], d5 = d[5], d6 = d[6], d7 = d[7];

	const float dr = m->dc.r;
	float xl = m->dc.x1[0], xr = m->dc.x1[1];
	float yl = m->dc.y1[0], yr = m->dc.y1[1];

	for (uint32_t i = 0; i < frames; i++, in += SP1M_IN_CH) {
		a0 += d0;
		a1 += d1;
		a2 += d2;
		a3 += d3;
		a4 += d4;
		a5 += d5;
		a6 += d6;
		a7 += d7;

		float l = (float)in[0] * a0 + (float)in[2] * a2 +
			  (float)in[4] * a4 + (float)in[6] * a6;
		float r = (float)in[1] * a1 + (float)in[3] * a3 +
			  (float)in[5] * a5 + (float)in[7] * a7;

		/* DC blocker, as dc_block_process() */
		yl = l - xl + dr * yl;
		yr = r - xr + dr * yr;
		xl = l;
		xr = r;
		l = yl;
		r = yr;

		float al = fabsf(l), ar = fabsf(r);

		if (al > MIXER_CLIP_KNEE || ar > MIXER_CLIP_KNEE) {
			clipped++;
			l = mixer_soft_clip(l);
			r = mixer_soft_clip(r);
			al = fabsf(l);
			ar = fabsf(r);
		}
		if (al > opk) {
			opk = al;
		}
		if (ar > opk) {
			opk = ar;
		}

		out[2 * i] = l;
		out[2 * i + 1] = r;
	}

	m->dc.x1[0] = xl;
	m->dc.x1[1] = xr;
	m->dc.y1[0] = yl;
	m->dc.y1[1] = yr;
	m->out_peak = opk;
	m->clipped += clipped;
}

void dc_block_init(struct dc_block *d, float fs, float fc)
{
	d->r = 1.0f - 2.0f * 3.14159265f * fc / fs;
	dc_block_reset(d);
}

void dc_block_reset(struct dc_block *d)
{
	d->x1[0] = d->x1[1] = 0.0f;
	d->y1[0] = d->y1[1] = 0.0f;
}

void dc_block_process(struct dc_block *d, float *buf, uint32_t frames)
{
	const float r = d->r;
	float xl = d->x1[0], xr = d->x1[1], yl = d->y1[0], yr = d->y1[1];

	for (uint32_t i = 0; i < frames; i++, buf += 2) {
		float l = buf[0], rr = buf[1];

		yl = l - xl + r * yl;
		yr = rr - xr + r * yr;
		xl = l;
		xr = rr;
		buf[0] = yl;
		buf[1] = yr;
	}

	d->x1[0] = xl;
	d->x1[1] = xr;
	d->y1[0] = yl;
	d->y1[1] = yr;
}

float mixer_take_peak(struct mixer *m)
{
	float pk = m->out_peak;

	m->out_peak = 0.0f;

	return pk;
}
