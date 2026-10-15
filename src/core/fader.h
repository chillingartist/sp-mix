/*
 * SPDX-License-Identifier: MIT
 *
 * Fader conditioning (median of 3, one-pole low pass, hysteresis) and
 * soft takeover ("pickup") for faders whose stored value no longer matches
 * their physical position.
 */

#ifndef SP1M_FADER_H_
#define SP1M_FADER_H_

#include "sp1m.h"

#define FADER_MAX	1023
/*
 * Raw SAADC range of the fader track (ends are clamped). Measured on an
 * SP-1: 0 at the bottom, 3700-3820 at the top, down to ~3620 while a ladder
 * button loads the shared supply.
 */
#define FADER_RAW_LO	60
#define FADER_RAW_HI	3600

struct fader {
	int16_t hist[3];
	uint8_t idx;
	float lpf;
	uint16_t out;
	bool init;
};

void fader_init(struct fader *f);

/*
 * Feed one raw sample; returns the conditioned position 0..FADER_MAX.
 * @alpha is the low-pass coefficient per sample (0..1].
 */
uint16_t fader_update(struct fader *f, int raw, float alpha);

/* Volume taper: bottom ~2% is silence, then roughly logarithmic to 0 dB. */
float fader_to_gain(uint16_t pos);

struct pickup {
	bool pending;
	uint16_t target;
	int16_t last;
	int16_t high;		/* highest position while pending below target */
};

/* Distance at which a fader counts as having reached its stored value */
#define PICKUP_TOLERANCE 12
/* Pulling a fader down by this much below its stored value takes over */
#define PICKUP_DOWN_STEP 24

/* Arm a pickup towards @target with the fader currently at @pos. */
void pickup_arm(struct pickup *p, uint16_t target, uint16_t pos);

/*
 * Track the fader; returns true once the fader has reached or crossed the
 * target, or has been pulled down while below it (and from then on): a
 * fader pulled down always makes the track quieter.
 */
bool pickup_update(struct pickup *p, uint16_t pos);

#endif /* SP1M_FADER_H_ */
