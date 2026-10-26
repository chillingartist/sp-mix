/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ryan Gilmore
 */

#include "fader.h"
#include "midi_map.h"
#include "scene.h"
#include "ui.h"

static void bind(struct midi_params *p, uint8_t channel, uint8_t kind,
		 uint16_t number, uint16_t param)
{
	const struct midi_in_bind b = { .t = { kind, channel, number }, .param = param };

	/* Fixed non-overlapping table, below the vendor's capacity limits. */
	(void)midi_params_bind(p, &b);
}

void sp1m_midi_params_init(struct midi_params *p, uint8_t channel)
{
	midi_params_init(p, SP1M_PARAM_COUNT, NULL, NULL, NULL);
	/* The faders' pickup is the UI's (ui_ext_volume): the table only
	 * parses and filters, nothing physical writes to it
	 */
	for (int id = 0; id < SP1M_PARAM_COUNT; id++) {
		midi_params_set_pickup(p, id, 0);
	}
	if (channel > 15) {
		return;
	}
	for (int t = 0; t < SP1M_TRACKS; t++) {
		bind(p, channel, MIDI_TGT_CC14, 20 + t, SP1M_PARAM_TRACK1_VOLUME + t);
		bind(p, channel, MIDI_TGT_CC14, 24 + t, SP1M_PARAM_TRACK1_BALANCE + t);
		bind(p, channel, MIDI_TGT_CC, 102 + t, SP1M_PARAM_TRACK1_MUTE + t);
		bind(p, channel, MIDI_TGT_CC, 106 + t, SP1M_PARAM_TRACK1_SOLO + t);
	}
	bind(p, channel, MIDI_TGT_CC14, 28, SP1M_PARAM_MASTER_VOLUME);
	const struct midi_in_bind recall = {
		.t = { MIDI_TGT_PROGRAM, channel, MIDI_NUM_ANY },
		.param = SP1M_PARAM_SCENE_RECALL, .event = 1, .min = 0, .max = 3
	};

	(void)midi_params_bind(p, &recall);
}

/* Rounded integer scaling between 0..from and 0..to */
static uint32_t rescale(uint32_t v, uint32_t from, uint32_t to)
{
	return (v * to + from / 2u) / from;
}

uint16_t sp1m_midi_to_vol(uint16_t value14)
{
	return (uint16_t)rescale(value14, MIDI_HR_MAX, FADER_MAX);
}

int sp1m_midi_to_balance(uint16_t value14, uint8_t res)
{
	/* A 7-bit sender's 64 is the centre, and so is a 14-bit 8192: the
	 * ends are reached from both, so each side has its own span
	 */
	int32_t v = res == 7 ? midi_narrow7(value14) : value14;
	int32_t c = res == 7 ? 64 : 8192;
	int32_t span = v <= c ? c : c - 1;
	int32_t d = (v - c) * BALANCE_MAX;

	return (int)((d + (d < 0 ? -span / 2 : span / 2)) / span);
}

int sp1m_midi_to_master_db(uint16_t value14)
{
	/* Off, then -60..0 dB in the UI's 1 dB steps, evenly spaced */
	return MASTER_DB_OFF +
	       (int)rescale(value14, MIDI_HR_MAX, MASTER_DB_MAX - MASTER_DB_OFF);
}

void sp1m_midi_changed(uint16_t param, uint16_t value14, uint8_t source, uint8_t res, void *ctx)
{
	struct sp1m_midi_context *c = ctx;
	const uint32_t now = c->in->now_ms;

	(void)source;
	if (param <= SP1M_PARAM_TRACK4_VOLUME) {
		int t = param - SP1M_PARAM_TRACK1_VOLUME;

		ui_ext_volume(c->ui, t, sp1m_midi_to_vol(value14), c->in->fader[t], now);
	} else if (param <= SP1M_PARAM_TRACK4_BALANCE) {
		ui_ext_balance(c->ui, param - SP1M_PARAM_TRACK1_BALANCE,
			       sp1m_midi_to_balance(value14, res), now);
	} else if (param <= SP1M_PARAM_TRACK4_MUTE) {
		ui_ext_mute(c->ui, param - SP1M_PARAM_TRACK1_MUTE,
			    value14 >= midi_widen7(64), now);
	} else if (param <= SP1M_PARAM_TRACK4_SOLO) {
		ui_ext_solo(c->ui, param - SP1M_PARAM_TRACK1_SOLO,
			    value14 >= midi_widen7(64), now);
	} else if (param == SP1M_PARAM_MASTER_VOLUME) {
		ui_set_master_db(c->ui, sp1m_midi_to_master_db(value14), now);
	} else if (param == SP1M_PARAM_SCENE_RECALL) {
		(void)ui_scene_recall(c->ui, value14, c->in->fader, now);
	}
}

static uint16_t balance_to14(int bal)
{
	return bal <= 0 ? (uint16_t)(8192 - rescale((uint32_t)-bal, BALANCE_MAX, 8192))
			: (uint16_t)(8192 + rescale((uint32_t)bal, BALANCE_MAX, 8191));
}

void sp1m_midi_sync(struct midi_params *p, const struct scene *cur)
{
	for (int t = 0; t < SP1M_TRACKS; t++) {
		uint8_t bit = (uint8_t)(1u << t);

		midi_params_preset(p, SP1M_PARAM_TRACK1_VOLUME + t,
				   (uint16_t)rescale(cur->vol[t], FADER_MAX, MIDI_HR_MAX));
		midi_params_preset(p, SP1M_PARAM_TRACK1_BALANCE + t, balance_to14(cur->bal[t]));
		midi_params_preset(p, SP1M_PARAM_TRACK1_MUTE + t, cur->mute & bit ? MIDI_HR_MAX : 0);
		midi_params_preset(p, SP1M_PARAM_TRACK1_SOLO + t, cur->solo & bit ? MIDI_HR_MAX : 0);
	}
	midi_params_preset(p, SP1M_PARAM_MASTER_VOLUME,
			   (uint16_t)rescale((uint32_t)(cur->master_db - MASTER_DB_OFF),
					     MASTER_DB_MAX - MASTER_DB_OFF, MIDI_HR_MAX));
}
