/*
 * SPDX-License-Identifier: MIT
 *
 * Stereo asynchronous sample rate converter for ratios close to 1.
 * Polyphase Kaiser-windowed sinc, ASRC_TAPS taps, ASRC_PHASES phases with
 * linear interpolation between neighbouring phases. Host simulation: error
 * against an ideal resampler is about -97 dB at 1 kHz, -109 dB at 5 kHz,
 * -95 dB at 10 kHz and -91 dB at 16 kHz; the response droops by about 0.7 dB
 * at 20 kHz. 20 taps rather than 24 save a sixth of the CPU time with the
 * window retuned (24 taps, beta 10: -102/-99/-93/-89 dB).
 *
 * Usage per output block:
 *   n = asrc_frames_needed(a, n_out, step);
 *   asrc_push(a, input, n);
 *   asrc_pull(a, output, n_out, step);
 *
 * @step is the read position increment per output frame in Q32.32, i.e.
 * input frames consumed per output frame. Above 1.0 the converter consumes
 * input faster than it produces output.
 */

#ifndef SP1M_ASRC_H_
#define SP1M_ASRC_H_

#include "sp1m.h"

#ifndef ASRC_TAPS
#define ASRC_TAPS		20
#endif
#ifndef ASRC_PHASE_BITS
#define ASRC_PHASE_BITS		7
#endif
#define ASRC_PHASES		(1 << ASRC_PHASE_BITS)
#define ASRC_HALF		(ASRC_TAPS / 2)
/* History needed before the read position */
#define ASRC_PRE		(ASRC_HALF - 1)
#define ASRC_MAX_OUT		256
#define ASRC_BUF_FRAMES		(ASRC_MAX_OUT + 2 * ASRC_TAPS + 8)

#define ASRC_ONE		((uint64_t)1 << 32)

struct asrc {
	/* Per phase and tap: the coefficient and its step to the next phase,
	 * interleaved so that the inner loop reads them together
	 */
	float coef[ASRC_PHASES][2 * ASRC_TAPS];
	float buf[2 * ASRC_BUF_FRAMES];	/* interleaved stereo history */
	uint32_t n;			/* frames in buf */
	uint64_t pos;			/* Q32.32 read position in buf */
};

/* Builds the filter table and resets the state. */
void asrc_init(struct asrc *a);

/* Clears the history; the group delay worth of silence is pre-filled. */
void asrc_reset(struct asrc *a);

/* Q32.32 step for a ratio offset in parts per million. */
uint64_t asrc_step_from_ppm(float ppm);

uint32_t asrc_frames_needed(const struct asrc *a, uint32_t n_out, uint64_t step);

/* Appends interleaved stereo frames. Returns frames accepted. */
uint32_t asrc_push(struct asrc *a, const float *in, uint32_t frames);

/*
 * Produces @n_out interleaved stereo frames. The caller must have pushed
 * asrc_frames_needed() frames first; missing input reads as silence.
 */
void asrc_pull(struct asrc *a, float *out, uint32_t n_out, uint64_t step);

/*
 * As asrc_pull(), written as I2S words: 24-bit samples, right aligned and
 * sign extended to 32 bits, saturated at full scale. Saves the separate
 * float to integer pass.
 */
void asrc_pull_i24(struct asrc *a, int32_t *out, uint32_t n_out, uint64_t step);

/*
 * Zero-copy alternative to asrc_push(): the producer writes up to *fit
 * interleaved frames at the returned pointer, then commits them.
 */
float *asrc_in_buf(struct asrc *a, uint32_t frames, uint32_t *fit);
void asrc_in_commit(struct asrc *a, uint32_t frames);

/* Group delay in input frames. */
#define ASRC_DELAY_FRAMES	ASRC_HALF

#endif /* SP1M_ASRC_H_ */
