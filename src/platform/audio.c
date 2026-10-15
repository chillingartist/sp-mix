/*
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <math.h>
#include <string.h>

#include <zephyr/drivers/i2s.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <cmsis_core.h>

#include "asrc.h"
#include "audio.h"
#include "codec.h"
#include "rate_est.h"
#include "ring.h"
#include "safety.h"
#include "seqbox.h"
#include "tstamp.h"

#define RING_FRAMES		1024
#define BLOCK_BYTES		(SP1M_BLOCK_FRAMES * 2 * sizeof(int32_t))
/* Most input frames one block can need: 64 * (1 + 1000 ppm) + taps */
#define MAX_IN_FRAMES		(SP1M_BLOCK_FRAMES + ASRC_TAPS + 4)
/* Ring fill to wait for before starting, in frames */
#define PREROLL_FRAMES		(CONFIG_SP1M_TARGET_FILL - SP1M_USB_FRAMES_NOM)
/* Above this the ring is cut back to PREROLL_FRAMES */
#define OVERRUN_FRAMES		(RING_FRAMES - 3 * SP1M_USB_FRAMES_MAX)
/* Blocks of fade-out once the host stops (~16 ms) */
#define FADE_BLOCKS		12
/* Empty blocks in a row before the stream is buffered up again */
#define STARVED_BLOCKS		4
#define LEVEL_BLOCKS		8
#define I2S_TIMEOUT_MS		20

/*
 * Cooperative and above the USB threads (usbd, udc_nrfx: K_PRIO_COOP(8)) and
 * the system work queue (CDC ACM): they can no longer stretch a block, which
 * on the first hardware run turned bursts of USB and console work into late
 * blocks and I2S underruns. A block (~0.5 ms) delays them instead: udc_nrfx
 * arms the next ISO OUT buffer after each packet and has about a frame for
 * it, so one block fits, two back to back would not. 'audio preempt' on the
 * console switches back for comparison.
 */
#define AUDIO_PRIO_COOP		K_PRIO_COOP(7)
#define AUDIO_PRIO_PREEMPT	K_PRIO_PREEMPT(0)

#define BLOCK_US		(SP1M_BLOCK_FRAMES * 1000000u / SP1M_FS)
#define TICKS_PER_US		(TSTAMP_HZ / 1000000u)

/* Every block in flight is queued in the driver while priming */
BUILD_ASSERT(CONFIG_SP1M_I2S_BLOCKS <= CONFIG_I2S_NRFX_TX_BLOCK_COUNT,
	     "raise CONFIG_I2S_NRFX_TX_BLOCK_COUNT");

enum audio_state {
	AUDIO_IDLE,
	AUDIO_PREROLL,
	AUDIO_RUN,
	AUDIO_FADE,
};

static const struct device *const i2s = DEVICE_DT_GET(DT_NODELABEL(i2s0));

K_MEM_SLAB_DEFINE_STATIC(tx_slab, BLOCK_BYTES, CONFIG_SP1M_I2S_BLOCKS + 1, 4);

static int16_t ring_mem[RING_FRAMES * SP1M_IN_CH] __aligned(4);
static struct frame_ring ring;

static struct mixer mixer;
static struct asrc asrc;
static struct rate_est est;
static bool running;		/* the audio thread exists */
static int init_err;
static uint32_t restart_fails;

/* Mixer input for frames the ring could not supply; stays zero */
static int16_t silence_in[MAX_IN_FRAMES * SP1M_IN_CH] __aligned(4);

/* Control thread -> audio thread */
static uint32_t params_seq;
static struct mix_params params_shared;
static struct mix_params params;

/* Audio thread -> control thread */
static uint32_t levels_seq;
static struct audio_levels levels_shared;
static bool levels_zero;	/* zero published since playback stopped */

/* USB thread -> audio thread: (packet count << 16) | USB frame number */
static atomic_t rx_stamp;
static atomic_t streaming;
static atomic_t usb_reset_req;
static atomic_t usb_active;
static atomic_t rx_dropped;	/* frames that did not fit into the ring */

static enum audio_state state;
static struct audio_stats stats;
static uint64_t block_ticks_sum;	/* blocks that played audio */
static uint32_t prev_start;		/* timer at the previous block start */
static bool have_prev_start;
static uint32_t last_gap_us;		/* this block's start after the previous */
static bool coop = true;

K_THREAD_STACK_DEFINE(audio_stack, 2048);
static struct k_thread audio_thread_data;

void audio_set_params(const struct mix_params *p)
{
	seqbox_write(&params_seq, &params_shared, p, sizeof(*p));
}

bool audio_get_levels(struct audio_levels *out)
{
	return seqbox_read(&levels_seq, out, &levels_shared, sizeof(*out));
}

void audio_set_streaming(bool on)
{
	atomic_set(&streaming, on ? 1 : 0);
}

void audio_usb_reset(void)
{
	atomic_set(&streaming, 0);
	atomic_set(&usb_reset_req, 1);
}

void audio_usb_active(bool on)
{
	atomic_set(&usb_active, on ? 1 : 0);
}

void audio_usb_rx(const void *data, uint32_t bytes, uint16_t frame_no)
{
	uint32_t frames = bytes / SP1M_USB_FRAME_BYTES;
	uint32_t written;
	atomic_val_t old;

	if (state == AUDIO_IDLE) {
		/* Nothing is consumed: keep the ring empty until we start. */
		return;
	}

	written = ring_write(&ring, data, frames);
	if (written < frames) {
		atomic_add(&rx_dropped, (atomic_val_t)(frames - written));
	}

	old = atomic_get(&rx_stamp);
	atomic_set(&rx_stamp, (atomic_val_t)((((uint32_t)old >> 16) + 1u) << 16 |
					    (frame_no & 0x7ffu)));
}

void audio_get_stats(struct audio_stats *out)
{
	*out = stats;
	out->state = (uint8_t)state;
	out->fill = ring_fill(&ring);
	out->fill_lin = est.fill_lin;
	out->ppm = est.ppm;
	out->ff_ppm = est.ff_ppm;
	out->ff_valid = est.ff_valid;
	out->sof_rejects = est.rejected;
	out->sof_relabels = est.relabeled;
	out->coop = coop;
	out->clipped = mixer.clipped;
	out->rx_dropped = (uint32_t)atomic_get(&rx_dropped);
	out->running = running;
	out->init_err = init_err;
	if (stats.run_blocks > 0) {
		out->block_us_avg = (uint32_t)(block_ticks_sum / stats.run_blocks /
					       TICKS_PER_US);
	}
}

void audio_reset_stats(void)
{
	memset(&stats, 0, sizeof(stats));
	block_ticks_sum = 0;
	atomic_set(&rx_dropped, 0);
	est.rejected = 0;
	est.relabeled = 0;
	mixer.clipped = 0;
}

/* Consistent snapshot of the ring fill and the newest packet's frame. */
static void read_fill(uint32_t *fill, uint16_t *rx_frame)
{
	atomic_val_t s1, s2;
	int tries = 0;

	do {
		s1 = atomic_get(&rx_stamp);
		*fill = ring_fill(&ring);
		s2 = atomic_get(&rx_stamp);
	} while (s1 != s2 && ++tries < 4);

	*rx_frame = (uint16_t)((uint32_t)s2 & 0x7ffu);
}

/* Word loop: the toolchain's memset goes byte by byte */
static void zero_words(void *p, size_t bytes)
{
	uint32_t *w = p;

	for (size_t i = 0; i < bytes / 4u; i++) {
		w[i] = 0;
	}
}

static void start_stream(void)
{
	ring_flush(&ring);
	rate_est_restart_fill(&est);
	state = AUDIO_PREROLL;
}

/* Not playing: publish a zero once so the output meter falls instead of
 * holding the last peak until the next stream
 */
static void levels_silent(uint32_t *level_div)
{
	if (!levels_zero) {
		struct audio_levels l = { .out = 0.0f };

		(void)mixer_take_peak(&mixer);
		seqbox_write(&levels_seq, &levels_shared, &l, sizeof(l));
		levels_zero = true;
	}
	*level_div = 0;
}

/* Returns true when the block played audio (RUN or FADE) */
static bool process_block(int32_t *out)
{
	static uint32_t fade_left;
	static uint32_t starved;
	static uint32_t level_div;
	uint32_t t_sof, t_blk, fill;
	uint16_t f_sof, rx_frame;
	struct mix_params p;
	bool want = atomic_get(&streaming) != 0;

	if (atomic_cas(&usb_reset_req, 1, 0)) {
		rate_est_reset(&est);
		state = AUDIO_IDLE;
	}

	/* USBD registers are only read while the bus is up */
	if (atomic_get(&usb_active)) {
		tstamp_sof(&t_sof, &f_sof);
		rate_est_sof(&est, t_sof, f_sof);
	}
	t_blk = tstamp_block();

	if (seqbox_read(&params_seq, &p, &params_shared, sizeof(p))) {
		params = p;
	}

	switch (state) {
	case AUDIO_IDLE:
		rate_est_observe_block(&est, t_blk);
		if (want) {
			start_stream();
		}
		levels_silent(&level_div);
		zero_words(out, BLOCK_BYTES);
		return false;

	case AUDIO_PREROLL:
		rate_est_observe_block(&est, t_blk);
		if (!want) {
			state = AUDIO_IDLE;
		} else if (ring_fill(&ring) >= PREROLL_FRAMES) {
			asrc_reset(&asrc);
			mixer_silence(&mixer);	/* fade in, DC blocker reset */
			starved = 0;
			state = AUDIO_RUN;
		}
		levels_silent(&level_div);
		zero_words(out, BLOCK_BYTES);
		return false;

	case AUDIO_RUN:
		if (!want) {
			fade_left = FADE_BLOCKS;
			state = AUDIO_FADE;
		}
		break;

	case AUDIO_FADE:
		if (want) {
			state = AUDIO_RUN;
		} else if (fade_left == 0 || --fade_left == 0) {
			/* Every block boundary must be observed, or the
			 * estimator counts this one as a block the thread is
			 * behind on
			 */
			rate_est_observe_block(&est, t_blk);
			state = AUDIO_IDLE;
			ring_flush(&ring);
			levels_silent(&level_div);
			zero_words(out, BLOCK_BYTES);
			return false;
		}
		break;
	}

	read_fill(&fill, &rx_frame);
	float ppm = rate_est_block(&est, t_blk, fill, rx_frame);
	uint64_t step = asrc_step_from_ppm(ppm);
	uint32_t need = asrc_frames_needed(&asrc, SP1M_BLOCK_FRAMES, step);

	if (need > MAX_IN_FRAMES) {
		need = MAX_IN_FRAMES;
	}

	/*
	 * Zero-copy: the mixer reads the frames in place from the ring and
	 * writes its stereo output straight into the ASRC history, which
	 * then produces the I2S words directly.
	 */
	struct ring_span span;
	uint32_t fit;
	float *dst = asrc_in_buf(&asrc, need, &fit);

	if (need > fit) {
		need = fit;	/* the ASRC buffer has room for several blocks */
	}

	uint32_t got = ring_peek(&ring, need, &span);

	if (got < need) {
		if (state == AUDIO_RUN) {
			stats.underruns++;
		}
		if (got == 0 && ++starved >= STARVED_BLOCKS && state == AUDIO_RUN) {
			/* The host went quiet without leaving the interface:
			 * buffer up again instead of crackling.
			 */
			start_stream();
		}
	} else {
		starved = 0;
	}

	const struct mix_params *mp = &params;

	if (state == AUDIO_FADE) {
		p = params;
		p.master_gain = 0.0f;
		mp = &p;
	}

	/* Gains move by the same amount whether a block is mixed in one
	 * piece or in several (the ring wrap, missing frames)
	 */
	mixer_process(&mixer, mp, span.p1, dst, span.n1);
	if (span.n2 > 0) {
		mixer_process(&mixer, mp, span.p2, &dst[2 * span.n1], span.n2);
	}
	if (got < need) {
		mixer_process(&mixer, mp, silence_in, &dst[2 * got], need - got);
	}
	ring_consume(&ring, got);
	asrc_in_commit(&asrc, need);
	asrc_pull_i24(&asrc, out, SP1M_BLOCK_FRAMES, step);

	if (ring_fill(&ring) > OVERRUN_FRAMES) {
		ring_drop(&ring, ring_fill(&ring) - PREROLL_FRAMES);
		stats.overruns++;
	}

	if (++level_div >= LEVEL_BLOCKS) {
		struct audio_levels l;

		level_div = 0;
		l.out = mixer_take_peak(&mixer);
		seqbox_write(&levels_seq, &levels_shared, &l, sizeof(l));
		levels_zero = false;
	}

	return true;
}

static int i2s_start(void)
{
	const struct i2s_config cfg = {
		.word_size = 24,
		.channels = 2,
		.format = I2S_FMT_DATA_FORMAT_I2S,
		.options = I2S_OPT_FRAME_CLK_SLAVE | I2S_OPT_BIT_CLK_SLAVE,
		.frame_clk_freq = SP1M_FS,
		.mem_slab = &tx_slab,
		.block_size = BLOCK_BYTES,
		.timeout = I2S_TIMEOUT_MS,
	};
	int primed = 0;
	int err;

	err = i2s_configure(i2s, I2S_DIR_TX, &cfg);
	if (err) {
		return err;
	}

	for (int i = 0; i < CONFIG_SP1M_I2S_BLOCKS; i++) {
		void *blk;

		if (k_mem_slab_alloc(&tx_slab, &blk, K_MSEC(5)) != 0) {
			break;
		}
		zero_words(blk, BLOCK_BYTES);
		if (i2s_write(i2s, blk, BLOCK_BYTES) != 0) {
			k_mem_slab_free(&tx_slab, blk);
			break;
		}
		primed++;
	}
	if (primed == 0) {
		return -ENOMEM;
	}

	return i2s_trigger(i2s, I2S_DIR_TX, I2S_TRIGGER_START);
}

static uint32_t ticks_to_us(uint32_t ticks)
{
	return ticks / TICKS_PER_US;
}

/*
 * Block statistics, the failing block included, in wall time from the timer
 * (the DWT cycle counter stops while the CPU sleeps). Block time only for
 * blocks that played audio: the silent ones take ~30 us and would hide the
 * real cost. The gap between block starts is one block period while the
 * thread keeps up; a longer one is how long it was held off. A gap of about
 * CONFIG_SP1M_I2S_BLOCKS periods runs the driver dry.
 */
static void account_block(bool played, uint32_t start, uint32_t ticks)
{
	stats.blocks++;
	if (played) {
		uint32_t us = ticks_to_us(ticks);

		stats.run_blocks++;
		block_ticks_sum += ticks;
		if (us > stats.block_us_max) {
			stats.block_us_max = us;
		}
		if (us > BLOCK_US) {
			stats.slow_blocks++;
		}
	}

	last_gap_us = 0;
	if (have_prev_start) {
		last_gap_us = ticks_to_us(start - prev_start);
		if (last_gap_us > stats.gap_max_us) {
			stats.gap_max_us = last_gap_us;
		}
		if (last_gap_us > BLOCK_US * 3u / 2u) {
			/* Periods late, rounded: 2 periods apart is 1 late */
			uint32_t k = (last_gap_us + BLOCK_US / 2u) / BLOCK_US - 1u;

			stats.late[MIN(k, ARRAY_SIZE(stats.late)) - 1u]++;
		}
	}
	prev_start = start;
	have_prev_start = true;
}

/* What the thread saw at the last I2S error, stall or failed start */
static void note_error(uint8_t kind, uint32_t ticks, int err)
{
	stats.err_kind = kind;
	stats.err_up_ms = k_uptime_get_32();
	stats.err_state = (uint8_t)state;
	stats.err_fill = ring_fill(&ring);
	stats.err_gap_us = last_gap_us;
	stats.err_us = ticks_to_us(ticks);
	stats.err_code = err;
}

static void i2s_restart(void)
{
	int err;

	(void)i2s_trigger(i2s, I2S_DIR_TX, I2S_TRIGGER_DROP);
	/* Block boundaries come back at another phase */
	rate_est_restart_output(&est);
	have_prev_start = false;
	if (state != AUDIO_IDLE) {
		/* Output was interrupted: buffer up again */
		start_stream();
	}
	err = i2s_start();
	if (err) {
		/* Back off so that a failing I2S cannot starve the threads
		 * below this one
		 */
		stats.i2s_start_errors++;
		stats.i2s_last_err = err;
		note_error(AUDIO_ERR_START, 0, err);
		restart_fails = MIN(restart_fails + 1u, 5u);
		k_msleep(2u << restart_fails);
	} else {
		restart_fails = 0;
	}
}

static void audio_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	int err = i2s_start();

	if (err) {
		stats.i2s_errors++;
		stats.i2s_last_err = err;
		note_error(AUDIO_ERR_START, 0, err);
	}

	for (;;) {
		void *blk;
		uint32_t t0, dt;
		bool played;

		safety_heartbeat(SAFETY_HB_AUDIO);

		/* A block frees up every 1.33 ms while LRCLK runs. */
		if (k_mem_slab_alloc(&tx_slab, &blk, K_MSEC(I2S_TIMEOUT_MS)) != 0) {
			stats.i2s_stalls++;
			last_gap_us = have_prev_start ?
				      ticks_to_us(tstamp_now() - prev_start) : 0;
			note_error(AUDIO_ERR_STALL, 0, -EAGAIN);
			i2s_restart();
			continue;
		}

		t0 = tstamp_now();
		played = process_block(blk);
		dt = tstamp_now() - t0;
		account_block(played, t0, dt);

		err = i2s_write(i2s, blk, BLOCK_BYTES);
		if (err) {
			/* Underrun towards the DMA: the driver stopped. */
			k_mem_slab_free(&tx_slab, blk);
			stats.i2s_errors++;
			note_error(AUDIO_ERR_WRITE, dt, err);
			i2s_restart();
			continue;
		}
	}
}

int audio_init(void)
{
	struct rate_est_cfg cfg;
	int err;

	/* Shared with the kernel's timing functions: enable, never reset */
	CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
	DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

	ring_init(&ring, ring_mem, RING_FRAMES, SP1M_IN_CH);
	mixer_init(&mixer, (float)SP1M_FS, 0.005f);
	asrc_init(&asrc);
	rate_est_default_cfg(&cfg);
	cfg.timer_hz = TSTAMP_HZ;
	cfg.target_fill = (float)CONFIG_SP1M_TARGET_FILL;
	rate_est_init(&est, &cfg);

	/* Without the thread nothing beats: the caller must not supervise it */
	if (!device_is_ready(i2s)) {
		init_err = -ENODEV;
		return init_err;
	}

	err = tstamp_init();
	if (err) {
		init_err = err;
		return err;
	}

	/* LRCLK comes from the CS42L42: bring the codecs up first. A codec
	 * failure is reported but the thread still runs (codec status says
	 * which chip and step).
	 */
	init_err = codec_init();

	k_thread_create(&audio_thread_data, audio_stack,
			K_THREAD_STACK_SIZEOF(audio_stack), audio_thread,
			NULL, NULL, NULL, coop ? AUDIO_PRIO_COOP : AUDIO_PRIO_PREEMPT,
			0, K_NO_WAIT);
	k_thread_name_set(&audio_thread_data, "audio");
	running = true;

	/* Silence is flowing; headphones can be unmuted for good. */
	k_msleep(10);
	codec_headphones_mute(false);

	return init_err;
}

bool audio_running(void)
{
	return running;
}

void audio_set_coop(bool on)
{
	coop = on;
	if (running) {
		k_thread_priority_set(&audio_thread_data,
				      on ? AUDIO_PRIO_COOP : AUDIO_PRIO_PREEMPT);
	}
}

void audio_shutdown(void)
{
	struct mix_params p = params;

	p.master_gain = 0.0f;
	audio_set_params(&p);
	k_msleep(30);
	codec_headphones_mute(true);
	(void)codec_speaker_enable(false);
	if (running) {
		k_thread_abort(&audio_thread_data);
		running = false;
		(void)i2s_trigger(i2s, I2S_DIR_TX, I2S_TRIGGER_DROP);
	}
	codec_power_down();
}
