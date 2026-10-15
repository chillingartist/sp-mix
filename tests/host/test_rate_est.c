/*
 * SPDX-License-Identifier: MIT
 *
 * Closed loop simulation: a host clock delivering packets at SOF, an output
 * clock pulling 64-frame blocks through the real ASRC accounting, and the
 * estimator steering between them.
 */

#include <math.h>
#include <string.h>

#include "asrc.h"
#include "rate_est.h"
#include "test.h"

static struct asrc sim_asrc;
/* When set: at this time the output restarts with a new block phase */
static double sim_restart_s;

struct sim_result {
	double final_ppm;
	double ppm_min, ppm_max;	/* after settling */
	double fill_min, fill_max;	/* ring fill before consumption, settled */
	uint32_t underruns;
};

static uint32_t lcg = 12345;

static double rnd(void)
{
	lcg = lcg * 1664525u + 1013904223u;
	return (double)(lcg >> 8) / 16777216.0;
}

/*
 * @host_ppm: host SOF clock error, @out_ppm: output crystal error,
 * @extra_ppm: host data rate relative to SOF (0 for SOF-locked hosts).
 *
 * Timing follows the nRF52840: data sent during USB frame N reaches the
 * firmware after SOF N+1 plus a random delay, tagged with frame N+1. Blocks
 * are processed a random short time after their I2S boundary.
 */
static struct sim_result simulate(double host_ppm, double out_ppm,
				  double extra_ppm, double seconds,
				  uint32_t late_every)
{
	uint32_t late_count = 0;
	const double tick_hz = 16e6;
	const double t_sof = tick_hz / 1000.0 * (1.0 + host_ppm * 1e-6);
	const double t_frame = tick_hz / 48000.0 * (1.0 + out_ppm * 1e-6);
	const double t_blk = 64.0 * t_frame;
	struct rate_est_cfg cfg;
	struct rate_est e;
	struct sim_result r = { .fill_min = 1e9, .fill_max = -1e9,
				.ppm_min = 1e9, .ppm_max = -1e9 };
	double next_sof = 0.37 * t_sof, blk = 0.0;
	double next_proc = blk + 0.002 * tick_hz;
	double deliver_t = -1.0;
	int deliver_n = 0, made_n = 0;
	uint16_t deliver_frame = 0, rx_frame = 0;
	double host_acc = 0.0;
	double fill = 0.0;
	double last_sof_t = 0.0;
	uint16_t frame_no = 0;
	bool started = false;
	bool restarted = false;
	float dummy[2 * 128];

	memset(dummy, 0, sizeof(dummy));
	rate_est_default_cfg(&cfg);
	rate_est_init(&e, &cfg);
	asrc_init(&sim_asrc);

	while (blk < seconds * tick_hz) {
		if (deliver_t >= 0.0 && deliver_t <= next_sof &&
		    deliver_t <= next_proc) {
			fill += deliver_n;
			rx_frame = deliver_frame;
			deliver_t = -1.0;
			continue;
		}

		if (next_sof <= next_proc) {
			frame_no = (uint16_t)((frame_no + 1) & 0x7ff);
			last_sof_t = next_sof;
			/* Deliver the previous frame's data shortly after */
			if (made_n > 0) {
				deliver_n = made_n;
				deliver_frame = frame_no;
				deliver_t = next_sof + (0.05 + 0.35 * rnd()) * 16000.0;
			}
			/* Host sends this frame's packet */
			host_acc += 48.0 * (1.0 + extra_ppm * 1e-6);
			made_n = (int)host_acc;
			host_acc -= made_n;
			next_sof += t_sof;
			continue;
		}

		if (sim_restart_s > 0.0 && !restarted && blk >= sim_restart_s * tick_hz) {
			/* I2S restart: boundaries move by a fraction of a block */
			restarted = true;
			blk += 0.37 * t_blk;
			next_proc = blk + 0.002 * tick_hz;
			rate_est_restart_output(&e);
			continue;
		}

		/* Block processing: read the latest SOF capture and the ring.
		 * Now and then the thread runs late: the next boundary has
		 * already passed and two blocks are processed back to back,
		 * both reading the newer capture.
		 */
		double cap = blk;

		if (late_every && (++late_count % late_every) == 0) {
			cap = blk + t_blk;
		} else if (late_every && (late_count % late_every) == 1) {
			cap = blk;
		}
		rate_est_sof(&e, (uint32_t)(uint64_t)last_sof_t, frame_no);
		if (!started && fill >= 200.0) {
			started = true;
			rate_est_restart_fill(&e);
		}

		float ppm = rate_est_block(&e, (uint32_t)(uint64_t)cap,
					   (uint32_t)fill, rx_frame);
		bool settled = blk > (seconds - 20.0) * tick_hz;

		if (settled) {
			if (fill < r.fill_min) {
				r.fill_min = fill;
			}
			if (fill > r.fill_max) {
				r.fill_max = fill;
			}
			if (ppm < r.ppm_min) {
				r.ppm_min = ppm;
			}
			if (ppm > r.ppm_max) {
				r.ppm_max = ppm;
			}
		}

		if (started) {
			uint64_t step = asrc_step_from_ppm(ppm);
			uint32_t need = asrc_frames_needed(&sim_asrc, 64, step);

			if (need > fill) {
				r.underruns++;
				need = (uint32_t)fill;
			}
			fill -= need;
			asrc_push(&sim_asrc, dummy, need);
			asrc_pull(&sim_asrc, dummy, 64, step);
		}

		r.final_ppm = ppm;
		blk += t_blk;
		next_proc = blk + (0.02 + 0.15 * rnd()) * 16000.0;
	}

	return r;
}

TEST(test_rate_est_locked_host)
{
	/* Host 120 ppm slow, crystal 40 ppm slow: ratio = (1+b)/(1+a) */
	struct sim_result r = simulate(120.0, -40.0, 0.0, 60.0, 0);
	double expect = ((1.0 - 40e-6) / (1.0 + 120e-6) - 1.0) * 1e6;

	printf("  locked: ppm %.2f (%.2f..%.2f, expect %.2f) fill %.0f..%.0f underruns %u\n",
	       r.final_ppm, r.ppm_min, r.ppm_max, expect, r.fill_min, r.fill_max,
	       r.underruns);
	CHECK_NEAR(r.ppm_min, expect, 3.0);
	CHECK_NEAR(r.ppm_max, expect, 3.0);
	CHECK(r.fill_min > 120.0);
	CHECK(r.fill_max < 260.0);
	CHECK(r.underruns == 0);
}

TEST(test_rate_est_unlocked_host)
{
	/* Host data runs 300 ppm off its own SOF: the PI loop must absorb it */
	struct sim_result r = simulate(0.0, 25.0, 300.0, 120.0, 0);
	double expect = ((1.0 + 25e-6) * (1.0 + 300e-6) - 1.0) * 1e6;

	printf("  unlocked: ppm %.2f (%.2f..%.2f, expect %.2f) fill %.0f..%.0f underruns %u\n",
	       r.final_ppm, r.ppm_min, r.ppm_max, expect, r.fill_min, r.fill_max,
	       r.underruns);
	CHECK_NEAR(r.ppm_min, expect, 10.0);
	CHECK_NEAR(r.ppm_max, expect, 10.0);
	CHECK(r.fill_min > 100.0);
	CHECK(r.fill_max < 280.0);
	CHECK(r.underruns == 0);
}

TEST(test_rate_est_late_blocks)
{
	/* Every 97th block reads a capture one block ahead */
	struct sim_result r = simulate(-200.0, 60.0, 0.0, 60.0, 97);
	double expect = ((1.0 + 60e-6) / (1.0 - 200e-6) - 1.0) * 1e6;

	printf("  late: ppm %.2f (%.2f..%.2f, expect %.2f) fill %.0f..%.0f underruns %u\n",
	       r.final_ppm, r.ppm_min, r.ppm_max, expect, r.fill_min, r.fill_max,
	       r.underruns);
	CHECK_NEAR(r.ppm_min, expect, 5.0);
	CHECK_NEAR(r.ppm_max, expect, 5.0);
	CHECK(r.underruns == 0);
}

TEST(test_rate_est_output_restart)
{
	/* Same clocks as the locked test, output restarted half way */
	sim_restart_s = 30.0;
	struct sim_result r = simulate(120.0, -40.0, 0.0, 60.0, 0);
	double expect = ((1.0 - 40e-6) / (1.0 + 120e-6) - 1.0) * 1e6;

	sim_restart_s = 0.0;
	printf("  restart: ppm %.2f (%.2f..%.2f, expect %.2f) underruns %u\n",
	       r.final_ppm, r.ppm_min, r.ppm_max, expect, r.underruns);
	/* The fill jump settles through the PI loop; without the restart
	 * the stale feed-forward window alone is about 11 ppm off
	 */
	CHECK_NEAR(r.ppm_min, expect, 5.0);
	CHECK_NEAR(r.ppm_max, expect, 5.0);
	CHECK(r.underruns == 0);
}

TEST(test_rate_est_rejects_bad_sof)
{
	struct rate_est_cfg cfg;
	struct rate_est e;

	rate_est_default_cfg(&cfg);
	rate_est_init(&e, &cfg);

	rate_est_sof(&e, 1000, 5);
	rate_est_sof(&e, 1000 + 16000, 6);
	CHECK(e.frames == 1);
	/* Frame number says 1 ms, capture says 3 ms: rejected */
	rate_est_sof(&e, 1000 + 64000, 7);
	CHECK(e.rejected == 1);
	CHECK(e.frames == 1);
	/* Timer wrap is handled */
	rate_est_reset(&e);
	rate_est_sof(&e, 0xffffc000u, 100);
	rate_est_sof(&e, 0xffffc000u + 32000u, 102);
	CHECK(e.rejected == 0);
	CHECK(e.frames == 2);
}

TEST(test_rate_est_sof_pairing)
{
	struct rate_est_cfg cfg;
	struct rate_est e;
	const uint32_t one = 16000;	/* timer ticks per USB frame */

	rate_est_default_cfg(&cfg);
	rate_est_init(&e, &cfg);

	rate_est_sof(&e, 1000, 100);
	for (uint32_t k = 1; k < 50; k++) {
		rate_est_sof(&e, 1000 + k * one, (uint16_t)(100 + k));
	}
	CHECK(e.frames == 49);

	/* Read just after a SOF: the counter already shows frame 51, the
	 * capture is still frame 50's. Labelled from the spacing, no reject.
	 */
	rate_est_sof(&e, 1000 + 50 * one, 151);
	CHECK(e.rejected == 0);
	CHECK(e.relabeled == 1);
	CHECK(e.frames == 50);
	/* Same capture once more with the counter ahead: ignored */
	rate_est_sof(&e, 1000 + 50 * one, 151);
	CHECK(e.frames == 50);
	/* And back in step */
	rate_est_sof(&e, 1000 + 52 * one, 152);
	rate_est_sof(&e, 1000 + 53 * one, 153);
	CHECK(e.rejected == 0);
	CHECK(e.frames == 53);
	CHECK(e.last_frame_no == 153);

	/* Two disagreements in a row: the counter was right after all,
	 * reject once and re-anchor on it
	 */
	rate_est_sof(&e, 1000 + 54 * one, 155);
	CHECK(e.relabeled == 2);
	CHECK(e.last_frame_no == 154);
	rate_est_sof(&e, 1000 + 55 * one, 156);
	CHECK(e.relabeled == 2);
	CHECK(e.rejected == 1);
	CHECK(e.last_frame_no == 156);
	rate_est_sof(&e, 1000 + 56 * one, 157);
	CHECK(e.rejected == 1);
	CHECK(e.last_frame_no == 157);
}
