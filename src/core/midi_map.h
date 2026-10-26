/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ryan Gilmore
 */

#ifndef SP1M_MIDI_MAP_H_
#define SP1M_MIDI_MAP_H_

#include "midi/midi_params.h"
#include "mixer.h"

enum sp1m_param {
	SP1M_PARAM_TRACK1_VOLUME,
	SP1M_PARAM_TRACK2_VOLUME,
	SP1M_PARAM_TRACK3_VOLUME,
	SP1M_PARAM_TRACK4_VOLUME,
	SP1M_PARAM_TRACK1_BALANCE,
	SP1M_PARAM_TRACK2_BALANCE,
	SP1M_PARAM_TRACK3_BALANCE,
	SP1M_PARAM_TRACK4_BALANCE,
	SP1M_PARAM_TRACK1_MUTE,
	SP1M_PARAM_TRACK2_MUTE,
	SP1M_PARAM_TRACK3_MUTE,
	SP1M_PARAM_TRACK4_MUTE,
	SP1M_PARAM_TRACK1_SOLO,
	SP1M_PARAM_TRACK2_SOLO,
	SP1M_PARAM_TRACK3_SOLO,
	SP1M_PARAM_TRACK4_SOLO,
	SP1M_PARAM_MASTER_VOLUME,
	SP1M_PARAM_SCENE_RECALL,
	SP1M_PARAM_COUNT
};

/* Resets the table, including callbacks. Channel is zero-based (0..15).
 * An invalid channel leaves an initialized table with no bindings.
 * Attach callbacks after initialization, before processing any messages.
 */
void sp1m_midi_params_init(struct midi_params *p, uint8_t channel);

struct ui;
struct ui_in;
struct scene;

/* Caller-owned callback context; keep it alive and keep in current (now_ms
 * and the fader positions) before delivering messages.
 */
struct sp1m_midi_context {
	struct ui *ui;
	const struct ui_in *in;
};

/* Attach as p->changed with p->ctx pointing to sp1m_midi_context after init.
 * Every change goes to the UI state (ui_ext_*, ui_set_master_db and
 * ui_scene_recall), so it is saved, recalled and capped like a fader or
 * button change. Balance respects res (7 or 14); scene events recall even on
 * repeats.
 */
void sp1m_midi_changed(uint16_t param, uint16_t value14, uint8_t source, uint8_t res, void *ctx);

/* Make the table hold the mixer's current state, without notifying. Call it
 * before delivering each tick's messages: a controller that resends a value
 * the device has since moved away from must not be ignored as unchanged.
 */
void sp1m_midi_sync(struct midi_params *p, const struct scene *cur);

/* The conversions, for the tests: to fader units, -100..100 and dB */
uint16_t sp1m_midi_to_vol(uint16_t value14);
int sp1m_midi_to_balance(uint16_t value14, uint8_t res);
int sp1m_midi_to_master_db(uint16_t value14);

#endif /* SP1M_MIDI_MAP_H_ */
