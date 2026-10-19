/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ryan Gilmore
 */

#include <string.h>

#include "test.h"
#include "fader.h"
#include "midi_map.h"
#include "scene.h"
#include "ui.h"

static void cc(struct midi_params *p, int channel, int number, int value)
{
	struct midi_msg m = { 0xb0 | channel, number, value, 3 };

	midi_params_on_msg(&m, MIDI_SRC_USB, p);
}

static void pair(struct midi_params *p, int channel, int number, int value)
{
	cc(p, channel, number, value >> 7);
	cc(p, channel, number + 32, value & 127);
}

static void program(struct midi_params *p, int channel, int number)
{
	struct midi_msg m = { 0xc0 | channel, number, 0, 2 };

	midi_params_on_msg(&m, MIDI_SRC_USB, p);
}

TEST(test_midi_map_bindings)
{
	struct midi_params p;
	const int base[] = { SP1M_PARAM_TRACK1_VOLUME, SP1M_PARAM_TRACK1_BALANCE,
		SP1M_PARAM_TRACK1_MUTE, SP1M_PARAM_TRACK1_SOLO };
	const int number[] = { 20, 24, 102, 106 };

	for (int channel = 0; channel < 16; channel++) {
		sp1m_midi_params_init(&p, channel);
		CHECK(p.nparams == SP1M_PARAM_COUNT);
		CHECK(p.nbinds == 18);
		for (int group = 0; group < 4; group++) {
			for (int t = 0; t < 4; t++) {
				int matches = 0;

				for (unsigned i = 0; i < p.nbinds; i++) {
					if (p.bind[i].param != base[group] + t) {
						continue;
					}
					matches++;
					CHECK(p.bind[i].t.kind == (group < 2 ? MIDI_TGT_CC14 : MIDI_TGT_CC));
					CHECK(p.bind[i].t.channel == channel);
					CHECK(p.bind[i].t.number == number[group] + t);
				}
				CHECK(matches == 1);
			}
		}
		CHECK(p.bind[16].param == SP1M_PARAM_MASTER_VOLUME);
		CHECK(p.bind[16].t.kind == MIDI_TGT_CC14);
		CHECK(p.bind[16].t.number == 28);
		CHECK(p.bind[16].t.channel == channel);
		CHECK(p.bind[17].param == SP1M_PARAM_SCENE_RECALL);
		CHECK(p.bind[17].t.kind == MIDI_TGT_PROGRAM);
		CHECK(p.bind[17].t.number == MIDI_NUM_ANY);
		CHECK(p.bind[17].t.channel == channel);
		CHECK(p.bind[17].event == 1);
		CHECK(p.bind[17].min == 0);
		CHECK(p.bind[17].max == 3);
	}
	sp1m_midi_params_init(&p, 16);
	CHECK(p.nbinds == 0);
}

TEST(test_midi_map_messages)
{
	struct midi_params p;
	struct ui u;
	struct ui_in in = { .now_ms = 100, .fader = { 100, 200, 300, 400 } };
	struct sp1m_midi_context ctx = { .ui = &u, .in = &in };

	ui_init(&u, NULL, in.fader, 0);
	sp1m_midi_params_init(&p, 3);
	p.changed = sp1m_midi_changed;
	p.ctx = &ctx;
	struct ui saved_ui = u;
	struct midi_param_state snapshot[SP1M_PARAM_COUNT];

	memcpy(snapshot, p.p, sizeof(snapshot));
	for (int n = 0; n < 128; n++) {
		cc(&p, 2, n, 127);
		program(&p, 2, n);
	}
	CHECK(memcmp(snapshot, p.p, sizeof(snapshot)) == 0);
	CHECK(memcmp(&saved_ui, &u, sizeof(u)) == 0);
	/* Unassigned controllers, including proposed scene save, ignored. */
	for (int n = 0; n < 128; n++) {
		if ((n >= 20 && n <= 28) || (n >= 52 && n <= 60) ||
		    (n >= 102 && n <= 109)) {
			continue;
		}
		cc(&p, 3, n, 127);
	}
	CHECK(memcmp(snapshot, p.p, sizeof(snapshot)) == 0);
	CHECK(memcmp(&saved_ui, &u, sizeof(u)) == 0);
	CHECK(!u.dirty);

	for (int t = 0; t < SP1M_TRACKS; t++) {
		uint8_t mask = (uint8_t)((1u << (t + 1)) - 1u);

		cc(&p, 3, 102 + t, 63);
		cc(&p, 3, 106 + t, 63);
		CHECK(u.st.current.mute == (mask >> 1));
		CHECK(u.st.current.solo == (mask >> 1));
		cc(&p, 3, 102 + t, 64);
		cc(&p, 3, 106 + t, 64);
		CHECK(u.st.current.mute == mask);
		CHECK(u.st.current.solo == mask);
		cc(&p, 3, 102 + t, 64); /* absolute state, not a toggle */
		cc(&p, 3, 106 + t, 127);
		cc(&p, 3, 106 + t, 127);
		CHECK(u.st.current.mute == mask);
		CHECK(u.st.current.solo == mask);
	}
	for (int t = 0; t < SP1M_TRACKS; t++) {
		uint8_t mask = (uint8_t)(15u & ~((1u << (t + 1)) - 1u));

		cc(&p, 3, 102 + t, 63);
		cc(&p, 3, 106 + t, 0);
		cc(&p, 3, 102 + t, 63);
		cc(&p, 3, 106 + t, 0);
		CHECK(u.st.current.mute == mask);
		CHECK(u.st.current.solo == mask);
	}

	cc(&p, 3, 28, 0);
	CHECK(u.st.current.master_db == MASTER_DB_OFF);
	cc(&p, 3, 28, 127);
	CHECK(u.st.current.master_db == MASTER_DB_MAX);
	struct ui_out out;

	ui_tick(&u, &in, &out);
	CHECK(u.st.current.master_db == MASTER_DB_MAX);
	in.now_ms += UI_TICK_MS;
	in.hp_maybe = true;
	ui_tick(&u, &in, &out);
	CHECK(u.st.current.master_db == UI_MASTER_SAFE_DB);

	for (int n = 0; n < SCENE_COUNT; n++) {
		for (int t = 0; t < SP1M_TRACKS; t++) {
			u.st.current.vol[t] = 500 + 100 * n + t;
			u.st.current.bal[t] = -60 + 20 * n + t;
		}
		u.st.current.mute = (uint8_t)(1u << n);
		u.st.current.solo = (uint8_t)(8u >> n);
		u.st.current.master_db = n == 2 ? -40 : -10;
		CHECK(ui_scene_save(&u, n, in.now_ms));
	}
	/* The first PC 0 and every repeat must recall. Mutate between sends
	 * so equality suppression cannot hide behind an unchanged scene.
	 */
	const int programs[] = { 0, 0, 2, 2, 1, 1, 3, 3 };

	for (unsigned i = 0; i < sizeof(programs) / sizeof(programs[0]); i++) {
		int n = programs[i];
		uint32_t gen = u.gen;

		u.st.current.vol[0] = 999;
		u.st.current.master_db = -30;
		ui_persist_done(&u, gen);
		in.now_ms++;
		program(&p, 3, n);
		for (int t = 0; t < SP1M_TRACKS; t++) {
			CHECK(u.st.current.vol[t] == u.st.slot[n].vol[t]);
			CHECK(u.st.current.bal[t] == u.st.slot[n].bal[t]);
			CHECK(u.vol_pick[t].pending);
		}
		CHECK(u.st.current.mute == u.st.slot[n].mute);
		CHECK(u.st.current.solo == u.st.slot[n].solo);
		CHECK(u.st.current.master_db == (n == 2 ? -40 : -30));
		CHECK(u.st.current.master_db <= -30);
		CHECK(u.last_scene == n);
		CHECK(u.gen == gen + 1);
		CHECK(u.confirm_until == in.now_ms + 600);
		CHECK(u.dirty);
		CHECK(midi_params_get(&p, SP1M_PARAM_SCENE_RECALL) == 0);
		CHECK(!midi_params_catching(&p, SP1M_PARAM_SCENE_RECALL));
	}
	saved_ui = u;
	memcpy(snapshot, p.p, sizeof(snapshot));
	for (int n = 4; n < 128; n++) {
		program(&p, 3, n);
	}
	CHECK(memcmp(snapshot, p.p, sizeof(snapshot)) == 0);
	CHECK(memcmp(&saved_ui, &u, sizeof(u)) == 0);
}

TEST(test_midi_map_curves)
{
	struct midi_params p;
	struct ui u;
	struct ui_in in = { .now_ms = 100, .fader = { 400, 400, 400, 400 } };
	struct sp1m_midi_context ctx = { .ui = &u, .in = &in };

	/* Fresh assemblers keep these MSB-only, including both end stops. */
	for (int n = 0; n < 128; n++) {
		ui_init(&u, NULL, in.fader, 0);
		sp1m_midi_params_init(&p, 0);
		p.changed = sp1m_midi_changed;
		p.ctx = &ctx;
		for (int t = 0; t < SP1M_TRACKS; t++) {
			cc(&p, 0, 20 + t, n);
			CHECK(u.st.current.vol[t] == sp1m_midi_to_vol(midi_widen7(n)));
			if (n == 0) {
				CHECK(u.st.current.vol[t] == 0);
			} else if (n == 127) {
				CHECK(u.st.current.vol[t] == FADER_MAX);
			}
		}
	}
	for (int t = 0; t < SP1M_TRACKS; t++) {
		pair(&p, 0, 20 + t, 10001 + t);
		CHECK(u.st.current.vol[t] == sp1m_midi_to_vol(10001 + t));
	}
	/* Fine movement is monotonic across the entire 14-bit range. */
	uint16_t last = 0;

	for (int v = 0; v <= (int)MIDI_HR_MAX; v++) {
		pair(&p, 0, 20, v);
		CHECK(u.st.current.vol[0] == sp1m_midi_to_vol(v));
		CHECK(u.st.current.vol[0] >= last);
		last = u.st.current.vol[0];
	}
	/* Review balance table: lone MSBs, explicit n/0 pairs and full scale. */
	const int msbs[] = { 0, 63, 64, 65, 127 };
	const int balances[] = { -100, -2, 0, 2, 100 };
	const int paired[] = { -100, -2, 0, 2, 98 };

	for (unsigned i = 0; i < sizeof(msbs) / sizeof(msbs[0]); i++) {
		ui_init(&u, NULL, in.fader, 0);
		sp1m_midi_params_init(&p, 0);
		p.changed = sp1m_midi_changed;
		p.ctx = &ctx;
		for (int t = 0; t < SP1M_TRACKS; t++) {
			cc(&p, 0, 24 + t, msbs[i]);
			CHECK(u.st.current.bal[t] == balances[i]);
			cc(&p, 0, 56 + t, 0);
			CHECK(u.st.current.bal[t] == paired[i]);
			pair(&p, 0, 24 + t, MIDI_HR_MAX);
			CHECK(u.st.current.bal[t] == BALANCE_MAX);
		}
	}
}

TEST(test_midi_map_pickup)
{
	struct midi_params p;
	struct ui u;
	struct ui_in in = { .now_ms = 100, .fader = { 100, 200, 300, 400 } };
	struct sp1m_midi_context ctx = { .ui = &u, .in = &in };

	ui_init(&u, NULL, in.fader, 0);
	sp1m_midi_params_init(&p, 0);
	p.changed = sp1m_midi_changed;
	p.ctx = &ctx;
	for (int id = 0; id < SP1M_PARAM_COUNT; id++) {
		CHECK(p.p[id].no_pickup);
		CHECK(!midi_params_catching(&p, id));
	}
	for (int t = 0; t < SP1M_TRACKS; t++) {
		CHECK(!u.vol_pick[t].pending);
		ui_persist_done(&u, u.gen);
		CHECK(!u.dirty);
		cc(&p, 0, 20 + t, 127);
		CHECK(u.st.current.vol[t] == FADER_MAX);
		CHECK(u.vol_pick[t].pending);
		CHECK(u.vol_pick[t].target == FADER_MAX);
		CHECK(u.vol_pick[t].last == in.fader[t]);
		CHECK(!midi_params_catching(&p, SP1M_PARAM_TRACK1_VOLUME + t));
		CHECK(u.dirty);
		CHECK(u.dirty_ms == in.now_ms);
	}
	/* A physical/UI change leaves the MIDI table stale. Sync must let the
	 * same controller value apply again, without itself changing the UI.
	 */
	u.st.current.vol[0] = 250;
	ui_persist_done(&u, u.gen);
	struct ui saved_ui = u;

	sp1m_midi_sync(&p, &u.st.current);
	CHECK(memcmp(&saved_ui, &u, sizeof(u)) == 0);
	CHECK(midi_params_get(&p, SP1M_PARAM_TRACK1_VOLUME) ==
	      (250u * MIDI_HR_MAX + FADER_MAX / 2) / FADER_MAX);
	in.now_ms++;
	cc(&p, 0, 20, 127);
	CHECK(u.st.current.vol[0] == FADER_MAX);
	CHECK(u.vol_pick[0].pending);
	CHECK(u.dirty);
	CHECK(u.dirty_ms == in.now_ms);
}
