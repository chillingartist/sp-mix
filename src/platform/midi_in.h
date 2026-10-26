/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ryan Gilmore
 *
 * MIDI control over USB (CONFIG_SP1M_MIDI): the USB-MIDI class hands its
 * packets to this module in the USB thread; the control thread applies them
 * to the UI state once per tick, to the DAW's side in controller mode
 * (ctrl.h), and sends what controller mode has for the host. docs/MIDI.md.
 */

#ifndef SP1M_MIDI_IN_H_
#define SP1M_MIDI_IN_H_

#include <stdint.h>

struct ctrl;
struct ui;
struct ui_in;

struct midi_in_stats {
	uint32_t packets;	/* USB-MIDI event packets received */
	uint32_t messages;	/* channel-voice messages queued */
	uint32_t dropped;	/* messages lost to a full queue */
	uint32_t applied;	/* messages handed to the parameter table */
	uint32_t ignored;	/* dropped in controller mode (echoes, scenes) */
	uint32_t sent;		/* controller mode: messages sent to the host */
	uint32_t overflow;	/* controller mode: messages lost before sending */
};

/* Before usbd_init(): installs the receive hook. */
void midi_in_init(void);

/* Control thread, after ui_init(): the table, its channel and callbacks.
 * Messages go to ctrl_ui(@c, @ui).
 */
void midi_in_start(struct ui *ui, const struct ui_in *in, struct ctrl *c);

/* Control thread, every tick before ctrl_tick(): apply what arrived. */
void midi_in_poll(void);

/* Control thread, after ctrl_tick(): send what controller mode has. */
void midi_out_flush(void);

void midi_in_get_stats(struct midi_in_stats *out);

#endif /* SP1M_MIDI_IN_H_ */
