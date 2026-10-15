/*
 * SPDX-License-Identifier: MIT
 */

#include <math.h>
#include <string.h>

#include "rate_est.h"

/* Reject SOF periods further than this from nominal (missed or bogus SOF). */
#define SOF_TOLERANCE_PPM 2000
/* The tolerance is computed as nom / (1e6 / ppm) */
_Static_assert(1000000 % SOF_TOLERANCE_PPM == 0, "1e6 / SOF_TOLERANCE_PPM");

void rate_est_default_cfg(struct rate_est_cfg *cfg)
{
	cfg->timer_hz = 16000000;
	cfg->usb_frames_nom = SP1M_USB_FRAMES_NOM;
	cfg->block_frames = SP1M_BLOCK_FRAMES;
	cfg->fs_out = (float)SP1M_FS;
	cfg->target_fill = 240.0f;	/* 5 ms, linearised */
	cfg->kp_ppm = 7.0f;		/* ~3 s loop time constant */
	cfg->ti_s = 20.0f;
	cfg->fill_tau_s = 0.05f;
	cfg->snap_period_s = 0.25f;
	cfg->max_ppm = 1000.0f;
}

void rate_est_init(struct rate_est *e, const struct rate_est_cfg *cfg)
{
	e->cfg = *cfg;
	rate_est_reset(e);
}

void rate_est_reset(struct rate_est *e)
{
	struct rate_est_cfg cfg = e->cfg;

	memset(e, 0, sizeof(*e));
	e->cfg = cfg;
}

void rate_est_restart_fill(struct rate_est *e)
{
	e->fill_init = false;
}

void rate_est_restart_output(struct rate_est *e)
{
	/* Block boundaries restart at an arbitrary phase: the block count
	 * history no longer lines up with the frames sent
	 */
	e->have_blk = false;
	e->backlog = 0;
	e->blocks_since_snap = 0;
	e->hist_n = 0;
	e->ff_valid = false;
}

static uint64_t unwrap(struct rate_est *e, uint32_t raw)
{
	/* Captures arrive in no particular order, but always within a few ms
	 * of each other: treat a small backwards step as an older capture.
	 */
	int32_t d = (int32_t)(raw - e->last_raw);

	if (d >= 0) {
		e->ext += (uint32_t)d;
		e->last_raw = raw;
		return e->ext;
	}

	return e->ext - (uint32_t)(-d);
}

void rate_est_sof(struct rate_est *e, uint32_t t, uint16_t frame_no)
{
	uint64_t ts;
	uint16_t df;

	frame_no &= 0x7ff;
	if (!e->have_sof && !e->have_blk) {
		e->last_raw = t;
		e->ext = t;
	}
	ts = unwrap(e, t);

	if (!e->have_sof) {
		e->have_sof = true;
		e->t_sof = ts;
		e->frames = 0;
		e->last_frame_no = frame_no;
		return;
	}

	df = (uint16_t)((frame_no - e->last_frame_no) & 0x7ff);
	if (df == 0) {
		return;
	}

	/* Sanity check the period; a mismatch means the frame number and the
	 * capture did not belong together, or SOFs went missing (suspend).
	 */
	const uint32_t one = e->cfg.timer_hz / 1000u;
	uint64_t dt = ts - e->t_sof;
	/* df < 2048, so 32 bits hold these: no library division */
	uint32_t nom = (uint32_t)df * one;
	uint32_t tol = nom / (1000000u / SOF_TOLERANCE_PPM) + 2u;

	/*
	 * FRAMECNTR and the PPI capture of the SOF event do not change at the
	 * same instant, so a read close to a SOF can pair a frame number with
	 * the capture of the neighbouring frame. The capture spacing is exact:
	 * when it says one frame more or less than the counter, label this
	 * capture from the spacing. If the very next observation disagrees
	 * again, it was the reference that was mispaired: fall through to the
	 * reject path below, which re-anchors on the counter.
	 */
	if (ts >= e->t_sof && dt <= (uint64_t)one * 64u) {
		uint32_t d32 = (uint32_t)dt;
		uint32_t df_t = (d32 + one / 2u) / one;
		uint32_t nom_t = df_t * one;
		uint32_t tol_t = nom_t / (1000000u / SOF_TOLERANCE_PPM) + 2u;
		bool fits = d32 + tol_t >= nom_t && d32 <= nom_t + tol_t;

		if (df_t == 0 && df == 1) {
			/* Same capture again, the counter is already ahead */
			return;
		}
		if (fits && df_t >= 1 &&
		    (df_t == (uint32_t)df + 1u || df_t + 1u == df) &&
		    !e->sof_relabeled) {
			e->sof_relabeled = true;
			e->relabeled++;
			e->t_sof = ts;
			e->frames += df_t;
			e->last_frame_no = (uint16_t)((e->last_frame_no + df_t) & 0x7ff);
			return;
		}
	}
	e->sof_relabeled = false;

	if (ts < e->t_sof || dt + tol < nom || dt > nom + tol) {
		e->rejected++;
		/* Restart the feed-forward window from this observation. */
		e->hist_n = 0;
		e->ff_valid = false;
		e->t_sof = ts;
		e->last_frame_no = frame_no;
		return;
	}

	e->t_sof = ts;
	e->frames += df;
	e->last_frame_no = frame_no;
}

static void update_ff(struct rate_est *e)
{
	struct rate_snap *s;
	const struct rate_snap *o, *n;
	uint32_t newest, oldest;

	if (!e->have_sof || !e->have_blk) {
		return;
	}

	newest = e->hist_head;
	s = &e->hist[newest];
	s->t_sof = e->t_sof;
	s->frames = e->frames;
	s->t_blk = e->t_blk;
	s->blocks = e->blocks;
	e->hist_head = (newest + 1) % RATE_EST_HIST;
	if (e->hist_n < RATE_EST_HIST) {
		e->hist_n++;
	}

	if (e->hist_n < 4) {
		return;
	}

	oldest = (newest + RATE_EST_HIST + 1 - e->hist_n) % RATE_EST_HIST;
	o = &e->hist[oldest];
	n = &e->hist[newest];

	int64_t d_tsof = (int64_t)(n->t_sof - o->t_sof);
	int64_t d_frames = (int64_t)(n->frames - o->frames);
	int64_t d_tblk = (int64_t)(n->t_blk - o->t_blk);
	int64_t d_out = (int64_t)(n->blocks - o->blocks) * e->cfg.block_frames;

	if (d_tsof <= 0 || d_frames <= 0 || d_tblk <= 0 || d_out <= 0) {
		return;
	}

	/*
	 * ratio = (usb_nom / T_usb) / (1 / T_out)
	 *       = usb_nom * d_tblk * d_frames / (d_out * d_tsof)
	 * ppm   = (num - den) / den * 1e6, exact in 64-bit integers.
	 */
	int64_t num = (int64_t)e->cfg.usb_frames_nom * d_tblk * d_frames;
	int64_t den = d_out * d_tsof;
	float ppm = (float)(num - den) / (float)den * 1e6f;

	if (fabsf(ppm) <= e->cfg.max_ppm) {
		e->ff_ppm = ppm;
		e->ff_valid = true;
	}
}

float rate_est_linear_fill(const struct rate_est *e, uint32_t fill,
			   uint16_t rx_frame_no)
{
	int32_t df;
	float phi;

	if (!e->have_sof || !e->have_blk) {
		return (float)fill;
	}

	/* USB frames started since the newest delivered packet */
	df = (int32_t)((e->last_frame_no - rx_frame_no) & 0x7ff);
	if (df >= 1024) {
		df -= 2048;
	}
	df = sp1m_clampi(df, -1, 3);

	/* Position of the block boundary being processed (the newest capture
	 * minus any backlog) within the current USB frame
	 */
	float t_nom = (float)e->cfg.block_frames * (float)e->cfg.timer_hz /
		      e->cfg.fs_out;

	phi = ((float)(int64_t)(e->t_blk - e->t_sof) - (float)e->backlog * t_nom) /
	      ((float)e->cfg.timer_hz / 1000.0f);
	phi = sp1m_clampf(phi, -1.5f, 1.5f);

	return (float)fill + (float)e->cfg.usb_frames_nom * ((float)df + phi);
}

void rate_est_observe_block(struct rate_est *e, uint32_t t)
{
	const struct rate_est_cfg *c = &e->cfg;
	float dt_blk = (float)c->block_frames / c->fs_out;
	uint32_t snap_blocks = (uint32_t)(c->snap_period_s / dt_blk + 0.5f);

	uint64_t ts;

	if (!e->have_sof && !e->have_blk) {
		e->last_raw = t;
		e->ext = t;
	}
	ts = unwrap(e, t);

	if (e->have_blk) {
		/*
		 * Count the block boundaries between two captures from their
		 * spacing instead of counting calls: a late call reads the
		 * newest capture, so calls and boundaries can be one apart.
		 * Captures are at most a few blocks apart, so rounding is
		 * unambiguous within the +-1000 ppm range.
		 */
		float t_nom = (float)c->block_frames * (float)c->timer_hz / c->fs_out;
		float nf = (float)(int64_t)(ts - e->t_blk) / t_nom;
		uint32_t n;

		if (nf < 0.5f) {
			/* Same capture as last time: this call catches up
			 * on a boundary that was already counted.
			 */
			if (e->backlog > 0) {
				e->backlog--;
			}
			return;
		}
		n = (uint32_t)(nf + 0.5f);
		e->blocks += n;
		/* More than one boundary since the last call: the thread
		 * runs late and will process the extra ones back to back.
		 * A long gap (output restarted) resynchronises instead.
		 */
		e->backlog = n <= 4 ? e->backlog + n - 1 : 0;
	}
	e->t_blk = ts;
	e->have_blk = true;

	if (++e->blocks_since_snap >= snap_blocks) {
		e->blocks_since_snap = 0;
		update_ff(e);
	}
}

float rate_est_block(struct rate_est *e, uint32_t t, uint32_t fill,
		     uint16_t rx_frame_no)
{
	const struct rate_est_cfg *c = &e->cfg;
	float dt_blk = (float)c->block_frames / c->fs_out;
	float err, ppm;

	rate_est_observe_block(e, t);

	float lin = rate_est_linear_fill(e, fill, rx_frame_no);

	e->fill_lin = lin;
	if (!e->fill_init) {
		e->fill_lpf = lin;
		e->fill_init = true;
	} else {
		e->fill_lpf += (lin - e->fill_lpf) * (dt_blk / c->fill_tau_s);
	}

	err = e->fill_lpf - c->target_fill;
	e->integ_ppm += c->kp_ppm * err * dt_blk / c->ti_s;
	e->integ_ppm = sp1m_clampf(e->integ_ppm, -0.6f * c->max_ppm,
				   0.6f * c->max_ppm);

	ppm = (e->ff_valid ? e->ff_ppm : 0.0f) + c->kp_ppm * err + e->integ_ppm;
	e->ppm = sp1m_clampf(ppm, -c->max_ppm, c->max_ppm);

	return e->ppm;
}
