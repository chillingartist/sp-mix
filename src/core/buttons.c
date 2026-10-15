/*
 * SPDX-License-Identifier: MIT
 */

#include "buttons.h"

struct band {
	int16_t lo;	/* inclusive */
	int16_t hi;	/* exclusive */
	uint16_t mask;
};

#define T(n) BTN_BIT(BTN_T##n)
#define PLAY BTN_BIT(BTN_PLAY)

/* Centre values in comments are the measured levels. */
static const struct band ladder1[] = {
	{    0,  110, 0 },
	{  110,  308, T(1) },			/*  213 */
	{  308,  488, T(2) },			/*  403 */
	{  488,  650, T(1) | T(2) },		/*  572 */
	{  650,  795, T(3) },			/*  733 */
	{  795,  925, T(1) | T(3) },		/*  862 */
	{  925, 1044, T(2) | T(3) },		/*  989 */
	{ 1044, 1154, T(1) | T(2) | T(3) },	/* 1100 */
	{ 1154, 1256, T(4) },			/* 1220 */
	{ 1256, 1347, T(1) | T(4) },		/* 1303 */
	{ 1347, 1430, T(2) | T(4) },		/* 1391 */
	{ 1430, 1509, T(1) | T(2) | T(4) },	/* 1470 */
	{ 1509, 1583, T(3) | T(4) },		/* 1548 */
	{ 1583, 1650, T(1) | T(3) | T(4) },	/* 1618 */
	{ 1650, 1713, T(2) | T(3) | T(4) },	/* 1683 */
	{ 1713, 1773, BTN_TRACK_MASK },		/* 1743 */
	{ 1773, 1840, PLAY },			/* 1798-1823 */
	{ 1840, 1886, PLAY | T(1) },		/* 1861 */
	{ 1886, 1957, PLAY | T(2) },		/* 1910 */
	{ 1957, 2081, PLAY | T(3) },		/* 2004 */
	{ 2081, 2267, PLAY | T(4) },		/* 2159 */
};

static const struct band ladder2[] = {
	{    0,  200, 0 },
	{  200,  560, BTN_BIT(BTN_BACK) },	/*  404 */
	{  560,  950, BTN_BIT(BTN_VOL_DOWN) },	/*  729 */
	{  950, 1500, BTN_BIT(BTN_FWD) },	/* 1220 */
	{ 1500, 1910, BTN_BIT(BTN_VOL_UP) },	/* 1820 */
	{ 1910, 2200, BTN_BIT(BTN_VOL_UP) | BTN_BIT(BTN_VOL_DOWN) }, /* 1998 */
};

static uint32_t lookup(const struct band *b, int n, int raw)
{
	for (int i = 0; i < n; i++) {
		if (raw >= b[i].lo && raw < b[i].hi) {
			return b[i].mask;
		}
	}

	return BTN_INVALID;
}

uint32_t ladder1_decode(int raw)
{
	if (raw < 0) {
		raw = 0;
	}

	return lookup(ladder1, (int)SP1M_ARRAY_SIZE(ladder1), raw);
}

uint32_t ladder2_decode(int raw)
{
	if (raw < 0) {
		raw = 0;
	}

	return lookup(ladder2, (int)SP1M_ARRAY_SIZE(ladder2), raw);
}

uint32_t buttons_from_ladders(int raw1, int raw2)
{
	uint32_t m2 = ladder2_decode(raw2);
	uint32_t m1;

	if (m2 != 0 && m2 != BTN_INVALID && raw1 >= 1509 && raw1 < 1840) {
		m1 = PLAY;
	} else {
		m1 = ladder1_decode(raw1);
	}

	if ((m1 | m2) & BTN_INVALID) {
		return BTN_INVALID;
	}

	return m1 | m2;
}

uint32_t btn_debounce_update(struct btn_debounce *d, uint32_t sample, uint8_t n)
{
	if (sample != d->candidate) {
		d->candidate = sample;
		d->count = 1;
	} else if (d->count < 255) {
		d->count++;
	}

	if (d->count >= n && !(sample & BTN_INVALID)) {
		d->stable = sample;
	}

	return d->stable;
}
