/*
 * SPDX-License-Identifier: MIT
 *
 * Button decoding. FUNCTION is a GPIO; everything else sits on two resistor
 * ladders read by the SAADC. The ladders are binary weighted, so every
 * combination of track buttons has its own level.
 *
 * Levels are raw 12-bit SAADC codes (gain 1/6, internal 0.6 V reference,
 * 20 us acquisition, ladders powered from P1.10) as measured on hardware by
 * the sp1-tape-looper project (Copyright (c) 2026 chattock, MIT; see
 * THIRD_PARTY_NOTICES.md).
 */

#ifndef SP1M_BUTTONS_H_
#define SP1M_BUTTONS_H_

#include "sp1m.h"

enum btn {
	BTN_T1,
	BTN_T2,
	BTN_T3,
	BTN_T4,
	BTN_PLAY,
	BTN_BACK,
	BTN_VOL_DOWN,
	BTN_FWD,
	BTN_VOL_UP,
	BTN_FN,
	BTN_COUNT,
};

#define BTN_BIT(b)		(1u << (b))
#define BTN_TRACK_MASK		0x0fu
#define BTN_LADDER1_MASK	(BTN_TRACK_MASK | BTN_BIT(BTN_PLAY))
#define BTN_LADDER2_MASK	(BTN_BIT(BTN_BACK) | BTN_BIT(BTN_VOL_DOWN) | \
				 BTN_BIT(BTN_FWD) | BTN_BIT(BTN_VOL_UP))
/* Ladder reading that matched no known level */
#define BTN_INVALID		0x80000000u

/* Raw ladder level to button mask (or BTN_INVALID). */
uint32_t ladder1_decode(int raw);
uint32_t ladder2_decode(int raw);

/*
 * Both ladders to one mask. Any ladder 2 press sags the shared supply, which
 * pushes PLAY down into the multi-track bands; while ladder 2 is pressed,
 * those bands read as PLAY.
 */
uint32_t buttons_from_ladders(int raw1, int raw2);

struct btn_debounce {
	uint32_t candidate;
	uint8_t count;
	uint32_t stable;
};

/*
 * Accepts a new mask only after it was seen @n times in a row (n >= 1).
 * BTN_INVALID samples (transitions between levels) never become stable.
 */
uint32_t btn_debounce_update(struct btn_debounce *d, uint32_t sample, uint8_t n);

#endif /* SP1M_BUTTONS_H_ */
