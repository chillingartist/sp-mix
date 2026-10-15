/*
 * SPDX-License-Identifier: MIT
 *
 * Clock ratio estimator driving the ASRC.
 *
 * Feed-forward: a free running timer captures the time of every USB SOF and
 * of every I2S block start (hardware PPI, no CPU jitter). Over a window of
 * a few seconds this gives the ratio of the USB frame clock to the output
 * sample clock with sub-ppm resolution.
 *
 * Feedback: a slow PI loop on the ring buffer fill keeps the latency at its
 * target and absorbs any host whose packet rate is not locked to SOF. Packets
 * arrive once per millisecond and blocks are taken every 1.33 ms, so the raw
 * fill is a staircase whose average shifts as the two clocks drift past each
 * other. The fill is therefore linearised with the SOF timestamps: frames of
 * USB frames that have started but not yet been delivered are counted as
 * arriving continuously.
 *
 * Output: ratio offset in ppm; positive means "consume input faster".
 */

#ifndef SP1M_RATE_EST_H_
#define SP1M_RATE_EST_H_

#include "sp1m.h"

#define RATE_EST_HIST		16

struct rate_est_cfg {
	uint32_t timer_hz;		/* capture timer frequency */
	uint32_t usb_frames_nom;	/* input frames per USB frame (48) */
	uint32_t block_frames;		/* output frames per block (64) */
	float fs_out;			/* nominal output rate */
	float target_fill;		/* ring fill set point, frames */
	float kp_ppm;			/* ppm per frame of fill error */
	float ti_s;			/* integral time */
	float fill_tau_s;		/* fill low-pass time constant */
	float snap_period_s;		/* feed-forward snapshot spacing */
	float max_ppm;			/* output clamp */
};

struct rate_snap {
	uint64_t t_sof;
	uint64_t frames;	/* unwrapped SOF frame count */
	uint64_t t_blk;
	uint64_t blocks;
};

struct rate_est {
	struct rate_est_cfg cfg;

	/* timer unwrap */
	uint32_t last_raw;
	uint64_t ext;

	/* latest observations, unwrapped */
	bool have_sof;
	uint64_t t_sof;
	uint64_t frames;
	uint16_t last_frame_no;
	bool sof_relabeled;	/* last frame number came from the capture */
	bool have_blk;
	uint64_t t_blk;
	uint64_t blocks;
	uint32_t backlog;	/* boundaries counted but not processed yet */
	uint32_t blocks_since_snap;

	struct rate_snap hist[RATE_EST_HIST];
	uint32_t hist_n;
	uint32_t hist_head;

	float ff_ppm;
	bool ff_valid;
	float fill_lin;		/* last linearised fill */
	float fill_lpf;
	bool fill_init;
	float integ_ppm;
	float ppm;

	uint32_t rejected;	/* SOF captures that did not fit */
	uint32_t relabeled;	/* frame number taken from the capture spacing */
};

void rate_est_default_cfg(struct rate_est_cfg *cfg);
void rate_est_init(struct rate_est *e, const struct rate_est_cfg *cfg);

/* Forget everything (USB reset or disconnect). Keeps the configuration. */
void rate_est_reset(struct rate_est *e);

/*
 * Stream restart on the same host: re-seed the fill filter but keep the
 * feed-forward window and the learned integral.
 */
void rate_est_restart_fill(struct rate_est *e);

/*
 * The output (I2S) restarted: block boundaries start at a new phase, so the
 * feed-forward window starts over. The learned integral is kept.
 */
void rate_est_restart_output(struct rate_est *e);

/*
 * Latest SOF capture: timer value and the 11-bit USB frame number belonging
 * to it. Call at least once per block; repeated values are ignored.
 */
void rate_est_sof(struct rate_est *e, uint32_t t, uint16_t frame_no);

/*
 * Start of an output block while no stream runs: keeps the feed-forward
 * estimate learning from SOF and block timestamps, leaves the PI loop alone.
 */
void rate_est_observe_block(struct rate_est *e, uint32_t t);

/*
 * Start of an output block.
 * @t: timer capture of the I2S block boundary
 * @fill: frames in the ring buffer when the block is processed
 * @rx_frame_no: USB frame number recorded when the newest packet in the ring
 *               was received
 * Returns the ratio offset in ppm to use for this block.
 */
float rate_est_block(struct rate_est *e, uint32_t t, uint32_t fill,
		     uint16_t rx_frame_no);

/* Fill as seen by the loop for the given observations (see above). */
float rate_est_linear_fill(const struct rate_est *e, uint32_t fill,
			   uint16_t rx_frame_no);

#endif /* SP1M_RATE_EST_H_ */
