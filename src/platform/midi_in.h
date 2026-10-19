/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ryan Gilmore
 *
 * MIDI control over USB (CONFIG_SP1M_MIDI), receive only: the USB-MIDI class
 * hands its packets to this module in the USB thread; the control thread
 * applies them to the UI state once per tick. docs/MIDI.md.
 */

#ifndef SP1M_MIDI_IN_H_
#define SP1M_MIDI_IN_H_

#include <stdint.h>

struct ui;
struct ui_in;

struct midi_in_stats {
	uint32_t packets;	/* USB-MIDI event packets received */
	uint32_t messages;	/* channel-voice messages queued */
	uint32_t dropped;	/* messages lost to a full queue */
	uint32_t applied;	/* messages handed to the parameter table */
};

/* Before usbd_init(): installs the receive hook. */
void midi_in_init(void);

/* Control thread, after ui_init(): the table, its channel and callbacks. */
void midi_in_start(struct ui *ui, const struct ui_in *in);

/* Control thread, every tick before ui_tick(): apply what arrived. */
void midi_in_poll(void);

void midi_in_get_stats(struct midi_in_stats *out);

#endif /* SP1M_MIDI_IN_H_ */
