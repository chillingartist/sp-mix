/*
 * SPDX-License-Identifier: MIT
 *
 * USB device: UAC2 8-channel adaptive playback + CDC ACM console.
 */

#ifndef SP1M_USB_H_
#define SP1M_USB_H_

#include <stdbool.h>
#include <stdint.h>

#include "sp1m.h"

struct usb_stats {
	bool configured;
	bool suspended;
	bool streaming;
	uint32_t packets;
	uint32_t zero_length;
	uint32_t partial;		/* size not a multiple of one frame */
	uint32_t frames47;
	uint32_t frames48;
	uint32_t frames49;
	uint32_t frames_other;
	uint32_t gaps;			/* USB frames without a packet while streaming */
	uint32_t late_rx;		/* packets handled a frame late (no loss) */
	uint32_t jumps;			/* frame number steps too large to be gaps */
	uint32_t no_buffer;
	int16_t peak[SP1M_IN_CH];	/* since the last read */
	uint32_t rx_calls;		/* packets handled since the last read */
	uint64_t rx_cycles;		/* CPU cycles spent on them */
	uint32_t rx_max;		/* longest single packet, cycles */
};

int usb_init(void);

bool usb_configured(void);
bool usb_streaming(void);

void usb_get_stats(struct usb_stats *out);

/* Per-track input peaks (0..1) since the last call, for the LED meters.
 * Needs CONFIG_SP1M_UAC2.
 */
void usb_take_track_peaks(float peak[SP1M_TRACKS]);
void usb_reset_stats(void);

/* Detach from the bus (power off path). */
void usb_shutdown(void);

#endif /* SP1M_USB_H_ */
