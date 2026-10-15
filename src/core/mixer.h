/*
 * SPDX-License-Identifier: MIT
 *
 * 8-in (4 stereo tracks) to 2-out mixer. Runs in the USB clock domain, before
 * the ASRC, so the resampler only ever sees two channels.
 */

#ifndef SP1M_MIXER_H_
#define SP1M_MIXER_H_

#include "sp1m.h"

/* Mixer parameters published by the control thread. */
struct mix_params {
	float track_gain[SP1M_TRACKS];	/* linear, 0..1 */
	float balance[SP1M_TRACKS];	/* -1 = left only .. +1 = right only */
	uint8_t mute_mask;		/* bit N = track N muted */
	uint8_t solo_mask;		/* bit N = track N soloed */
	float master_gain;		/* linear */
};

/*
 * First-order DC blocker for interleaved stereo. Keeps a DC offset from the
 * host (or a full-scale low-frequency mix) off the speaker coil.
 */
struct dc_block {
	float r;
	float x1[2];
	float y1[2];
};

void dc_block_init(struct dc_block *d, float fs, float fc);
void dc_block_reset(struct dc_block *d);
/* In place; mixer_process() has the same filter built in. */
void dc_block_process(struct dc_block *d, float *buf, uint32_t frames);

/* DC blocker corner of the mix */
#define MIXER_DC_HZ 5.0f

struct mixer {
	float fs;
	float tau_s;
	float gl[SP1M_TRACKS];
	float gr[SP1M_TRACKS];
	float master;
	struct dc_block dc;
	/* Max-hold output level, cleared by mixer_take_peak() */
	float out_peak;
	uint32_t clipped;	/* output samples that reached the soft clipper */
};

/* Soft clipper engages above this absolute level. */
#define MIXER_CLIP_KNEE 0.85f

void mixer_init(struct mixer *m, float fs, float tau_s);

/* Snap all gains (and the DC blocker) to silence; the next blocks ramp up
 * from there.
 */
void mixer_silence(struct mixer *m);

/*
 * Per-track target gains. A track is audible when it is soloed, or when
 * nothing is soloed and it is not muted (solo overrides mute).
 */
void mixer_target_gains(const struct mix_params *p, float gl[SP1M_TRACKS],
			float gr[SP1M_TRACKS]);

/* Balance law for stereo tracks: 0 dB at centre, cosine fade on one side. */
float mixer_balance_gain(float balance, bool right);

float mixer_soft_clip(float x);

/*
 * Mix @frames interleaved 8-channel frames into interleaved stereo float.
 * Gains move towards their targets with a one-pole response (tau_s) and are
 * interpolated linearly across the block, so fader steps do not zipper. The
 * sum goes through the DC blocker, then the soft clipper: in the other order
 * the blocker's overshoot on clipped bass would pass full scale.
 */
void mixer_process(struct mixer *m, const struct mix_params *p,
		   const int16_t *in, float *out, uint32_t frames);

/* Read and clear the output peak. Call from the thread running the mixer.
 * (Track meters come from the USB packets.)
 */
float mixer_take_peak(struct mixer *m);

#endif /* SP1M_MIXER_H_ */
