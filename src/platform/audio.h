/*
 * SPDX-License-Identifier: MIT
 *
 * Real-time audio path: USB ring buffer -> mixer -> ASRC -> I2S.
 */

#ifndef SP1M_AUDIO_H_
#define SP1M_AUDIO_H_

#include <stdbool.h>
#include <stdint.h>

#include "mixer.h"

struct audio_levels {
	float out;			/* output peak, 0..1 */
};

enum {
	AUDIO_ERR_NONE,
	AUDIO_ERR_WRITE,		/* the driver ran dry: block too late */
	AUDIO_ERR_STALL,		/* no free block for 20 ms: no LRCLK */
	AUDIO_ERR_START,		/* the I2S driver did not start */
};

struct audio_stats {
	uint8_t state;			/* enum audio_state */
	uint32_t fill;			/* ring fill, frames */
	float fill_lin;			/* linearised fill seen by the loop */
	float ppm;			/* ratio offset applied */
	float ff_ppm;			/* feed-forward estimate */
	bool ff_valid;
	uint32_t underruns;
	uint32_t overruns;		/* ring cut back after running full */
	uint32_t rx_dropped;		/* received frames that did not fit */
	uint32_t i2s_errors;
	uint32_t i2s_stalls;
	uint32_t sof_rejects;		/* SOF captures that did not fit */
	uint32_t sof_relabels;		/* frame number taken from the capture */
	uint32_t clipped;
	uint32_t blocks;		/* processed */
	uint32_t run_blocks;		/* ... of them playing audio */
	uint32_t slow_blocks;		/* playing blocks over one block period */
	uint32_t block_us_max;		/* playing blocks, wall time */
	uint32_t block_us_avg;
	uint32_t gap_max_us;		/* longest time between block starts */
	uint32_t late[3];		/* blocks started 1, 2, 3+ periods late */
	uint8_t err_kind;		/* last error: AUDIO_ERR_* */
	uint8_t err_state;		/* ... audio state then */
	int err_code;			/* ... driver error */
	uint32_t err_up_ms;		/* ... uptime */
	uint32_t err_fill;		/* ... ring fill */
	uint32_t err_gap_us;		/* ... time since the previous block start */
	uint32_t err_us;		/* ... time of the failing block */
	bool coop;			/* audio thread above the USB threads */
	uint32_t i2s_start_errors;	/* restarts that failed (backed off) */
	int i2s_last_err;
	bool running;			/* the audio thread exists */
	int init_err;			/* audio_init() result */
};

/*
 * Codecs, I2S and the audio thread. Returns the first error; the thread may
 * run anyway (audio_running()), e.g. when only a codec failed.
 */
int audio_init(void);

/* The audio thread exists and beats the audio heartbeat. */
bool audio_running(void);

/* Audio thread above (cooperative, default) or below the USB threads. */
void audio_set_coop(bool on);

/* Control thread: mixer parameters for the next blocks. */
void audio_set_params(const struct mix_params *p);

/* Control thread: newest level meters (peaks since the last call). */
bool audio_get_levels(struct audio_levels *out);

/* USB: host enabled or disabled the streaming interface. */
void audio_set_streaming(bool on);

/* USB: bus reset, suspend or disconnect. */
void audio_usb_reset(void);

/* USB: configured and not suspended, so SOFs arrive. */
void audio_usb_active(bool on);

/* USB: one received packet, @frame_no = USB frame number at reception. */
void audio_usb_rx(const void *data, uint32_t bytes, uint16_t frame_no);

void audio_get_stats(struct audio_stats *out);
void audio_reset_stats(void);

/* Silence, stop I2S and power the codecs down (power off path). */
void audio_shutdown(void);

#endif /* SP1M_AUDIO_H_ */
