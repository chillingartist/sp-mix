/*
 * SPDX-License-Identifier: MIT
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "ui.h"

#define B(x)		BTN_BIT(BTN_##x)
#define TRACK(t)	BTN_BIT(BTN_T1 + (t))
/* Outside every button bit: a chord carrying it never counts as a tap */
#define UI_CHORD_CANCEL	0x80000000u

/* Meter fall-off per tick: about 20 dB in 300 ms */
#define METER_DECAY	0.985f
#define METER_FLOOR_DB	(-48.0f)
/* Track LED level of an audible but silent track */
#define LED_IDLE	14

static bool after(uint32_t now, uint32_t t)
{
	return (int32_t)(now - t) >= 0;
}

static bool blink(uint32_t now, uint32_t period_ms)
{
	return ((now / (period_ms / 2)) & 1u) == 0;
}

static void mark_dirty(struct ui *u, uint32_t now, bool urgent)
{
	u->dirty = true;
	u->dirty_ms = now;
	u->gen++;
	if (urgent) {
		u->dirty_urgent = true;
	}
}

void ui_init(struct ui *u, const struct persist *loaded,
	     const uint16_t fader[SP1M_TRACKS], uint32_t now_ms)
{
	memset(u, 0, sizeof(*u));

	if (loaded != NULL) {
		u->st = *loaded;
	} else {
		persist_default(&u->st);
		for (int t = 0; t < SP1M_TRACKS; t++) {
			u->st.current.vol[t] = fader[t];
		}
	}

	scene_sanitize(&u->st.current);
	for (int s = 0; s < SCENE_COUNT; s++) {
		scene_sanitize(&u->st.slot[s]);
	}
	/* Never power up loud, whatever was stored */
	if (u->st.current.master_db > UI_MASTER_SAFE_DB) {
		u->st.current.master_db = UI_MASTER_SAFE_DB;
	}

	for (int t = 0; t < SP1M_TRACKS; t++) {
		pickup_arm(&u->vol_pick[t], u->st.current.vol[t], fader[t]);
		u->last_fader[t] = fader[t];
	}

	u->last_scene = -1;
	u->last_activity_ms = now_ms;
	u->raw_busy_ms = now_ms;
}

bool ui_scene_save(struct ui *u, int slot, uint32_t now_ms)
{
	if (slot < 0 || slot >= SCENE_COUNT) {
		return false;
	}

	u->st.slot[slot] = u->st.current;
	u->st.slot_valid |= (uint8_t)(1u << slot);
	u->last_scene = (int8_t)slot;
	u->confirm_until = now_ms + 600;
	mark_dirty(u, now_ms, true);

	return true;
}

bool ui_scene_recall(struct ui *u, int slot, const uint16_t fader[SP1M_TRACKS],
		     uint32_t now_ms)
{
	if (slot < 0 || slot >= SCENE_COUNT ||
	    !(u->st.slot_valid & (1u << slot))) {
		return false;
	}

	/* A scene may lower the master volume but never raise it */
	int8_t master = u->st.current.master_db;

	u->st.current = u->st.slot[slot];
	if (u->st.current.master_db > master) {
		u->st.current.master_db = master;
	}
	for (int t = 0; t < SP1M_TRACKS; t++) {
		pickup_arm(&u->vol_pick[t], u->st.current.vol[t], fader[t]);
		u->bal_active[t] = false;
	}
	u->last_scene = (int8_t)slot;
	u->confirm_until = now_ms + 600;
	mark_dirty(u, now_ms, false);

	return true;
}

void ui_set_master_db(struct ui *u, int db, uint32_t now_ms)
{
	db = sp1m_clampi(db, MASTER_DB_OFF, MASTER_DB_MAX);
	if (db != u->st.current.master_db) {
		u->st.current.master_db = (int8_t)db;
		mark_dirty(u, now_ms, false);
	}
	u->vol_show_until = now_ms + UI_VOL_SHOW_MS;
}

void ui_ext_volume(struct ui *u, int track, uint16_t vol, uint16_t fader,
		   uint32_t now_ms)
{
	if (track < 0 || track >= SP1M_TRACKS) {
		return;
	}
	vol = (uint16_t)sp1m_clampi(vol, 0, FADER_MAX);
	if (vol != u->st.current.vol[track]) {
		u->st.current.vol[track] = vol;
		mark_dirty(u, now_ms, false);
	}
	if (!u->bal_active[track]) {
		pickup_arm(&u->vol_pick[track], vol, fader);
	}
}

void ui_ext_balance(struct ui *u, int track, int bal, uint32_t now_ms)
{
	if (track < 0 || track >= SP1M_TRACKS) {
		return;
	}
	bal = sp1m_clampi(bal, -BALANCE_MAX, BALANCE_MAX);
	if (bal != u->st.current.bal[track]) {
		u->st.current.bal[track] = (int8_t)bal;
		mark_dirty(u, now_ms, false);
	}
}

static void ext_bit(struct ui *u, uint8_t *mask, int track, bool on,
		    uint32_t now_ms)
{
	uint8_t bit;

	if (track < 0 || track >= SP1M_TRACKS) {
		return;
	}
	bit = (uint8_t)(1u << track);
	if (((*mask & bit) != 0) != on) {
		*mask ^= bit;
		mark_dirty(u, now_ms, false);
	}
}

void ui_ext_mute(struct ui *u, int track, bool on, uint32_t now_ms)
{
	ext_bit(u, &u->st.current.mute, track, on, now_ms);
}

void ui_ext_solo(struct ui *u, int track, bool on, uint32_t now_ms)
{
	ext_bit(u, &u->st.current.solo, track, on, now_ms);
}

void ui_set_spk_mode(struct ui *u, enum spk_mode mode, uint32_t now_ms)
{
	if (u->st.current.spk_mode != (uint8_t)mode) {
		u->st.current.spk_mode = (uint8_t)mode;
		mark_dirty(u, now_ms, false);
	}
}

void ui_set_charge_off(struct ui *u, bool off, uint32_t now_ms)
{
	if (u->st.charge_off != (off ? 1 : 0)) {
		u->st.charge_off = off ? 1 : 0;
		mark_dirty(u, now_ms, true);
	}
}

bool ui_persist_due(const struct ui *u, uint32_t now_ms)
{
	if (!u->dirty) {
		return false;
	}

	return u->dirty_urgent || after(now_ms, u->dirty_ms + UI_PERSIST_QUIET_MS);
}

void ui_persist_snapshot(const struct ui *u, struct persist *out, uint32_t *gen)
{
	*out = u->st;
	*gen = u->gen;
}

void ui_persist_done(struct ui *u, uint32_t gen)
{
	if (gen == u->gen) {
		u->dirty = false;
		u->dirty_urgent = false;
	}
}

bool ui_take_power_off(struct ui *u)
{
	bool r = u->req_power_off;

	u->req_power_off = false;

	return r;
}

bool ui_take_dfu(struct ui *u)
{
	bool r = u->req_dfu;

	u->req_dfu = false;

	return r;
}

static void master_step(struct ui *u, bool up, uint32_t now)
{
	int db = u->st.current.master_db;

	if (up) {
		db = db <= MASTER_DB_OFF ? MASTER_DB_MIN : db + 1;
	} else {
		db = db <= MASTER_DB_MIN ? MASTER_DB_OFF : db - 1;
	}
	db = sp1m_clampi(db, MASTER_DB_OFF, MASTER_DB_MAX);

	if (db != u->st.current.master_db) {
		u->st.current.master_db = (int8_t)db;
		mark_dirty(u, now, false);
	}
	u->vol_show_until = now + UI_VOL_SHOW_MS;
}

static bool track_audible(const struct scene *s, int t)
{
	if (s->solo != 0) {
		return (s->solo & (1u << t)) != 0;
	}

	return (s->mute & (1u << t)) == 0;
}

static uint8_t level_to_led(float level)
{
	float db, x;

	if (level < 1e-5f) {
		return 0;
	}

	db = 20.0f * log10f(level);
	x = sp1m_clampf((db - METER_FLOOR_DB) / -METER_FLOOR_DB, 0.0f, 1.0f);

	return (uint8_t)(LED_IDLE + x * (255 - LED_IDLE));
}

/* Fill @n LEDs as a bar of @x in 0..1, the top LED partially lit. */
static void bar(uint8_t *led, int n, float x)
{
	float f = sp1m_clampf(x, 0.0f, 1.0f) * (float)n;

	for (int i = 0; i < n; i++) {
		float v = sp1m_clampf(f - (float)i, 0.0f, 1.0f);

		led[i] = (uint8_t)(v * 255.0f);
	}
}

static void render_leds(struct ui *u, const struct ui_in *in, uint8_t *led)
{
	const struct scene *s = &u->st.current;
	const uint32_t now = in->now_ms;
	uint8_t *tl = &led[0];
	uint8_t *pl = &led[SP1M_TRACKS];

	/* Track LEDs */
	for (int t = 0; t < SP1M_TRACKS; t++) {
		uint8_t v;

		if (u->t14_active) {
			uint32_t held = now - u->t14_ms;

			v = (uint8_t)(held >= UI_HOLD_DFU_MS ? 255 :
				      held * 255u / UI_HOLD_DFU_MS);
		} else if (u->scene_layer) {
			if (u->last_scene == t) {
				v = (!after(now, u->confirm_until) && !blink(now, 120)) ?
				    0 : 255;
			} else {
				v = (u->st.slot_valid & (1u << t)) ? 50 : 0;
			}
		} else if (u->vol_pick[t].pending) {
			v = blink(now, 1000) ? 90 : 0;
		} else if (s->solo & (1u << t)) {
			v = blink(now, 250) ? 255 : 20;
		} else if (!track_audible(s, t)) {
			v = 0;
		} else {
			v = level_to_led(u->track_level[t]);
			if (v < LED_IDLE) {
				v = LED_IDLE;
			}
		}
		tl[t] = v;
	}

	/* Playback LEDs */
	uint32_t fn_held = now - u->press_ms[BTN_FN];

	if ((u->prev_buttons & B(FN)) && u->fn_alone && !u->fn_fired &&
	    fn_held >= 500) {
		/* Power-off countdown: 4 LEDs going out one by one */
		uint32_t left = fn_held >= UI_HOLD_OFF_MS ? 0 : UI_HOLD_OFF_MS - fn_held;
		uint32_t lit = (left * 4 + UI_HOLD_OFF_MS - 1) / UI_HOLD_OFF_MS;

		for (uint32_t i = 0; i < 4; i++) {
			pl[i] = i < lit ? 255 : 0;
		}
	} else if (!after(now, u->underrun_until)) {
		memset(pl, 255, 4);
	} else if (u->scene_layer) {
		uint32_t ph = now % 1000;
		uint8_t v = (uint8_t)(30 + (ph < 500 ? ph : 1000 - ph) * 120 / 500);

		memset(pl, v, 4);
	} else if (!after(now, u->vol_show_until)) {
		bar(pl, 4, (float)(s->master_db - MASTER_DB_OFF) /
			   (float)(MASTER_DB_MAX - MASTER_DB_OFF));
	} else if (u->st.led_mode == LED_MODE_STATUS) {
		pl[0] = in->usb_configured ? 255 : 0;
		pl[1] = in->streaming ? 255 : 0;
		pl[2] = in->hp_present ? 255 : 0;
		pl[3] = in->charging ? (blink(now, 2000) ? 255 : 40) : 0;
	} else {
		static const float thr_db[4] = { -30.0f, -18.0f, -9.0f, -3.0f };
		float db = u->out_level > 1e-5f ? 20.0f * log10f(u->out_level) :
						  -120.0f;

		for (int i = 0; i < 4; i++) {
			pl[i] = db >= thr_db[i] ? 200 : 0;
		}
	}
}

void ui_tick(struct ui *u, const struct ui_in *in, struct ui_out *out)
{
	const uint32_t now = in->now_ms;
	const uint32_t b = in->buttons & ((1u << BTN_COUNT) - 1u);
	const uint32_t prev = u->prev_buttons;
	const uint32_t pressed = b & ~prev;
	const uint32_t released = prev & ~b;
	struct scene *cur = &u->st.current;

	if (prev == 0 && b != 0) {
		u->chord = 0;
	}
	u->chord |= b;
	/* The release forced by lost inputs is no tap: poison the chord
	 * until everything is up again
	 */
	if (in->inputs_lost) {
		u->chord |= UI_CHORD_CANCEL;
	}

	for (int i = 0; i < BTN_COUNT; i++) {
		if (pressed & BTN_BIT(i)) {
			u->press_ms[i] = now;
		}
	}

	if (pressed | released) {
		u->last_activity_ms = now;
	}

	/* The raw reading shows a press (and its sag) a few scans before the
	 * debounced mask does
	 */
	const bool pressed_db = (b & (BTN_LADDER1_MASK | BTN_LADDER2_MASK)) != 0;

	if (!in->ladder_busy || pressed_db) {
		u->raw_busy_ms = now;
	}
	const bool ladder_down = pressed_db ||
				 (in->ladder_busy && now - u->raw_busy_ms < UI_RAW_BUSY_MAX_MS);

	if (ladder_down) {
		u->ladder_free_ms = now;
	}
	const bool faders_settled = !ladder_down &&
				    now - u->ladder_free_ms >= UI_SAG_HOLDOFF_MS;
	for (int t = 0; t < SP1M_TRACKS; t++) {
		if (abs((int)in->fader[t] - (int)u->last_fader[t]) > 8) {
			u->last_activity_ms = now;
			u->last_fader[t] = in->fader[t];
		}
	}

	/* Recovery: exactly Track 1 + Track 4 on the track ladder, any mode. */
	if ((b & BTN_LADDER1_MASK) == (TRACK(0) | TRACK(3))) {
		if (!u->t14_active) {
			u->t14_active = true;
			u->t14_ms = now;
		} else if (now - u->t14_ms >= UI_HOLD_DFU_MS) {
			u->req_dfu = true;
		}
	} else {
		u->t14_active = false;
	}

	/* Power off: FUNCTION held alone */
	if (pressed & B(FN)) {
		u->fn_alone = (b == B(FN));
		u->fn_fired = false;
	}
	if (b & B(FN)) {
		if (b & ~B(FN)) {
			u->fn_alone = false;
		}
		if (u->fn_alone && !u->fn_fired &&
		    now - u->press_ms[BTN_FN] >= UI_HOLD_OFF_MS) {
			u->fn_fired = true;
			u->req_power_off = true;
		}
	}

	/* Track keys and faders */
	for (int t = 0; t < SP1M_TRACKS; t++) {
		const uint32_t bit = TRACK(t);
		const uint16_t f = in->fader[t];

		if (pressed & bit) {
			u->hold_fader[t] = f;
			u->hold_bal[t] = cur->bal[t];
			u->bal_active[t] = false;
		}

		if (b & bit) {
			if (u->scene_layer) {
				continue;
			}
			if (now - u->press_ms[BTN_T1 + t] < UI_SAG_SETTLE_MS) {
				/* Reading still drops with the supply: re-anchor */
				u->hold_fader[t] = f;
				continue;
			}
			if (!u->bal_active[t] &&
			    abs((int)f - (int)u->hold_fader[t]) >= UI_BAL_ENTER) {
				/* Relative from here on, so balance does not jump */
				u->bal_active[t] = true;
				u->hold_fader[t] = f;
				u->hold_bal[t] = cur->bal[t];
			}
			if (u->bal_active[t]) {
				int nb = u->hold_bal[t] +
					 (int)lroundf((float)((int)f - (int)u->hold_fader[t]) *
						      UI_BAL_PER_FADER);

				nb = sp1m_clampi(nb, -BALANCE_MAX, BALANCE_MAX);
				if (nb != cur->bal[t]) {
					cur->bal[t] = (int8_t)nb;
					mark_dirty(u, now, false);
				}
			}
			continue;
		}

		if (released & bit) {
			const bool was_bal = u->bal_active[t];
			const bool fn = (u->chord & B(FN)) != 0;
			const bool tap = !was_bal &&
					 now - u->press_ms[BTN_T1 + t] <= UI_TAP_MAX_MS &&
					 (u->chord & ~(bit | B(FN))) == 0;

			u->bal_active[t] = false;
			if (was_bal) {
				/* The fader now sits at a balance position. */
				pickup_arm(&u->vol_pick[t], cur->vol[t], f);
			}

			if (tap) {
				if (u->scene_layer) {
					if (fn) {
						ui_scene_save(u, t, now);
					} else {
						ui_scene_recall(u, t, in->fader, now);
					}
					u->scene_layer_ms = now;
				} else if (fn) {
					cur->solo ^= (uint8_t)bit;
					mark_dirty(u, now, false);
				} else {
					cur->mute ^= (uint8_t)bit;
					mark_dirty(u, now, false);
				}
			}
			continue;
		}

		if (!faders_settled) {
			continue;
		}
		if (pickup_update(&u->vol_pick[t], f) && cur->vol[t] != f) {
			cur->vol[t] = f;
			mark_dirty(u, now, false);
		}
	}

	/* PLAY: scene layer */
	if ((released & B(PLAY)) && u->chord == B(PLAY) &&
	    now - u->press_ms[BTN_PLAY] <= UI_TAP_MAX_MS) {
		u->scene_layer = !u->scene_layer;
		u->scene_layer_ms = now;
	}
	if (u->scene_layer && b != 0) {
		u->scene_layer_ms = now;
	}
	if (u->scene_layer && now - u->scene_layer_ms > UI_SCENE_LAYER_MS) {
		u->scene_layer = false;
	}

	/* Master volume */
	{
		const bool up = (b & B(VOL_UP)) != 0;
		const bool dn = (b & B(VOL_DOWN)) != 0;

		if (up != dn) {
			if (pressed & (B(VOL_UP) | B(VOL_DOWN))) {
				master_step(u, up, now);
				u->vol_next_ms = now + UI_VOL_REPEAT_DELAY_MS;
			} else if (after(now, u->vol_next_ms)) {
				master_step(u, up, now);
				u->vol_next_ms += UI_VOL_REPEAT_MS;
			}
		}
	}

	/* BACK / FWD: LED mode, or speaker mode with FUNCTION */
	for (int k = 0; k < 2; k++) {
		const int btn = k == 0 ? BTN_BACK : BTN_FWD;
		const uint32_t bit = BTN_BIT(btn);

		if ((released & bit) && (u->chord & ~(bit | B(FN))) == 0 &&
		    now - u->press_ms[btn] <= UI_TAP_MAX_MS) {
			if (u->chord & B(FN)) {
				ui_set_spk_mode(u, cur->spk_mode == SPK_AUTO ?
						   SPK_OFF : SPK_AUTO, now);
			} else {
				u->st.led_mode ^= 1u;
				mark_dirty(u, now, false);
			}
		}
	}

	/* Nothing to mix for: switch off after a while */
	if (!in->usb_configured &&
	    now - u->last_activity_ms >= UI_AUTO_OFF_MS) {
		u->req_power_off = true;
		u->last_activity_ms = now;
	}

	/* Meters */
	for (int t = 0; t < SP1M_TRACKS; t++) {
		u->track_level[t] *= METER_DECAY;
		if (in->track_peak[t] > u->track_level[t]) {
			u->track_level[t] = in->track_peak[t];
		}
	}
	u->out_level *= METER_DECAY;
	if (in->out_peak > u->out_level) {
		u->out_level = in->out_peak;
	}
	if (in->underrun) {
		u->underrun_until = now + 60;
	}

	u->prev_buttons = b;

	/* Headphones plugged in: whatever the speaker needed is too loud. On
	 * the first reading that says so, not after the debounce
	 */
	bool hp = in->hp_present || in->hp_maybe;

	if (hp && !u->hp_prev && cur->master_db > UI_MASTER_SAFE_DB) {
		cur->master_db = UI_MASTER_SAFE_DB;
		mark_dirty(u, now, false);
	}
	u->hp_prev = hp;

	/* Outputs */
	for (int t = 0; t < SP1M_TRACKS; t++) {
		out->mix.track_gain[t] = fader_to_gain(cur->vol[t]);
		out->mix.balance[t] = (float)cur->bal[t] / (float)BALANCE_MAX;
	}
	out->mix.mute_mask = cur->mute;
	out->mix.solo_mask = cur->solo;
	out->mix.master_gain = master_db_to_gain(cur->master_db);
	out->speaker_on = cur->spk_mode == SPK_AUTO && !in->hp_present;

	render_leds(u, in, out->led);
}
