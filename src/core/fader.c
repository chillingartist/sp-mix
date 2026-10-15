/*
 * SPDX-License-Identifier: MIT
 */

#include <math.h>
#include <stdlib.h>

#include "fader.h"

/* Output moves only when the filtered value is this far away. */
#define HYSTERESIS 3.0f
/* Positions below this are silence */
#define DEAD_ZONE (FADER_MAX / 50)

void fader_init(struct fader *f)
{
	f->idx = 0;
	f->lpf = 0.0f;
	f->out = 0;
	f->init = false;
}

static int median3(int a, int b, int c)
{
	if (a > b) {
		int t = a;

		a = b;
		b = t;
	}
	if (b > c) {
		b = c;
	}

	return a > b ? a : b;
}

uint16_t fader_update(struct fader *f, int raw, float alpha)
{
	float pos, target;
	int m;

	if (!f->init) {
		f->hist[0] = f->hist[1] = f->hist[2] = (int16_t)raw;
	} else {
		f->hist[f->idx] = (int16_t)raw;
		f->idx = (uint8_t)((f->idx + 1) % 3);
	}
	m = median3(f->hist[0], f->hist[1], f->hist[2]);

	pos = (float)(sp1m_clampi(m, FADER_RAW_LO, FADER_RAW_HI) - FADER_RAW_LO) *
	      (float)FADER_MAX / (float)(FADER_RAW_HI - FADER_RAW_LO);

	if (!f->init) {
		f->lpf = pos;
		f->out = (uint16_t)(pos + 0.5f);
		f->init = true;
		return f->out;
	}

	f->lpf += (pos - f->lpf) * alpha;
	target = f->lpf;

	/* Always reach the end stops exactly. */
	if (target < 1.0f) {
		f->out = 0;
	} else if (target > (float)FADER_MAX - 1.0f) {
		f->out = FADER_MAX;
	} else if (fabsf(target - (float)f->out) >= HYSTERESIS) {
		f->out = (uint16_t)(target + 0.5f);
	}

	return f->out;
}

float fader_to_gain(uint16_t pos)
{
	float x;

	if (pos <= DEAD_ZONE) {
		return 0.0f;
	}

	x = (float)(pos - DEAD_ZONE) / (float)(FADER_MAX - DEAD_ZONE);

	/* x^3: -18 dB at half travel, -60 dB at 10% */
	return x * x * x;
}

void pickup_arm(struct pickup *p, uint16_t target, uint16_t pos)
{
	p->target = target;
	p->last = (int16_t)pos;
	p->high = (int16_t)pos;
	p->pending = abs((int)pos - (int)target) > PICKUP_TOLERANCE;
}

bool pickup_update(struct pickup *p, uint16_t pos)
{
	if (p->pending) {
		int d_now = (int)pos - (int)p->target;
		int d_last = (int)p->last - (int)p->target;

		if ((int)pos > p->high) {
			p->high = (int16_t)pos;
		}
		if (abs(d_now) <= PICKUP_TOLERANCE ||
		    (d_now > 0) != (d_last > 0) ||
		    (d_now < 0 && (int)pos < p->high - PICKUP_DOWN_STEP)) {
			p->pending = false;
		}
	}
	p->last = (int16_t)pos;

	return !p->pending;
}
