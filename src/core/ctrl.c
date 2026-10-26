/*
 * SPDX-License-Identifier: MIT
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "ctrl.h"

#define B(x)		BTN_BIT(BTN_##x)
/* Outside every button bit: a chord carrying it never counts as a tap */
#define CHORD_CANCEL	0x80000000u
#define UNKNOWN		0xffu

/* Control ids: CTRL_VOL + track, ... */
enum { CTRL_VOL = 0, CTRL_BAL = 4, CTRL_MUTE = 8, CTRL_SOLO = 12 };
static const uint8_t cc_base[4] = { 20, 24, 102, 106 };

enum { ANIM_NONE, ANIM_ENTER, ANIM_LEAVE, ANIM_SENT };
static const uint16_t anim_len[] = { 0, 800, 800, 160 };

/* Brightness that marks controller mode on the first playback LED */
#define CTRL_LED_MARK	40

static bool after(uint32_t now, uint32_t t)
{
	return (int32_t)(now - t) >= 0;
}

uint8_t ctrl_vol7(uint16_t fader)
{
	if (fader > FADER_MAX) {
		fader = FADER_MAX;
	}

	return (uint8_t)(((uint32_t)fader * 127u + FADER_MAX / 2u) / FADER_MAX);
}

uint8_t ctrl_bal7(int bal)
{
	/* 64 is the centre: 64 steps to the left, 63 to the right */
	if (bal <= 0) {
		bal = bal < -BALANCE_MAX ? BALANCE_MAX : -bal;
		return (uint8_t)(64 - (bal * 64 + BALANCE_MAX / 2) / BALANCE_MAX);
	}
	if (bal > BALANCE_MAX) {
		bal = BALANCE_MAX;
	}

	return (uint8_t)(64 + (bal * 63 + BALANCE_MAX / 2) / BALANCE_MAX);
}

/* The 7-bit volume of a fader that was at @prev: it moves only once the
 * fader is CTRL_HYST units past the step, so a fader resting on a step
 * boundary does not flood the host
 */
static uint8_t vol7_hyst(uint16_t fader, uint8_t prev)
{
	const uint8_t n = ctrl_vol7(fader);
	const int32_t f2 = (int32_t)fader * 254;

	if (prev > 127 || n == prev) {
		return n;
	}
	if (n > prev) {
		return f2 >= (2 * prev + 1) * FADER_MAX + 254 * CTRL_HYST ? n : prev;
	}

	return f2 <= (2 * prev - 1) * FADER_MAX - 254 * CTRL_HYST ? n : prev;
}

static uint8_t bit7(uint8_t mask, int t)
{
	return (mask & (1u << t)) ? 127 : 0;
}

static void queue(struct ctrl *c, uint8_t status, uint8_t d1, uint8_t d2)
{
	if (c->ob_count >= CTRL_OUTBOX) {
		c->overflow++;
		return;
	}

	struct midi_msg *m = &c->outbox[(c->ob_head + c->ob_count) % CTRL_OUTBOX];

	m->status = status;
	m->d1 = d1;
	m->d2 = d2;
	m->len = 3;
	c->ob_count++;
	c->sent++;
}

static void send_id(struct ctrl *c, int id, uint32_t now)
{
	queue(c, (uint8_t)(0xb0 | c->channel), (uint8_t)(cc_base[id / 4] + id % 4),
	      c->last[id]);
	c->known[id] = c->last[id];
	c->due_ms[id] = now + CTRL_RATE_MS;
	c->touch_ms[id] = now;
	c->touched[id] = true;
}

/* A button: on, then off, so the DAW toggles once */
static void send_pulse(struct ctrl *c, int id, uint32_t now)
{
	const uint8_t status = (uint8_t)(0xb0 | c->channel);
	const uint8_t cc = (uint8_t)(cc_base[id / 4] + id % 4);

	queue(c, status, cc, 127);
	queue(c, status, cc, 0);
	c->known[id] = c->last[id];
	c->touch_ms[id] = now;
	c->touched[id] = true;
}

/* The four track volumes, whatever was sent before */
static void dump(struct ctrl *c, uint32_t now)
{
	for (int t = 0; t < SP1M_TRACKS; t++) {
		send_id(c, CTRL_VOL + t, now);
	}
	c->dump_pending = false;
}

/* What changed in rui since the last tick came over MIDI: the DAW has it */
static void take_external(struct ctrl *c)
{
	const struct scene *s = &c->rui.st.current;

	for (int t = 0; t < SP1M_TRACKS; t++) {
		uint8_t v[4] = { UNKNOWN, UNKNOWN, UNKNOWN, UNKNOWN };

		if (s->vol[t] != c->seen.vol[t]) {
			v[0] = ctrl_vol7(s->vol[t]);
		}
		if (s->bal[t] != c->seen.bal[t]) {
			v[1] = ctrl_bal7(s->bal[t]);
		}
		if (bit7(s->mute, t) != bit7(c->seen.mute, t)) {
			v[2] = bit7(s->mute, t);
		}
		if (bit7(s->solo, t) != bit7(c->seen.solo, t)) {
			v[3] = bit7(s->solo, t);
		}
		for (int k = 0; k < 4; k++) {
			if (v[k] != UNKNOWN) {
				c->last[k * 4 + t] = v[k];
				c->known[k * 4 + t] = v[k];
			}
		}
	}
	c->seen = *s;
}

/* What changed in rui during the tick came from the controls */
static void take_physical(struct ctrl *c, uint32_t now)
{
	const struct scene *s = &c->rui.st.current;

	for (int t = 0; t < SP1M_TRACKS; t++) {
		const uint8_t v[4] = {
			vol7_hyst(s->vol[t], c->last[CTRL_VOL + t]),
			ctrl_bal7(s->bal[t]),
			bit7(s->mute, t),
			bit7(s->solo, t),
		};

		for (int k = 0; k < 4; k++) {
			const int id = k * 4 + t;

			if (v[k] != c->last[id]) {
				c->last[id] = v[k];
				c->touch_ms[id] = now;
				c->touched[id] = true;
			}
		}
	}
	c->seen = *s;
}

static void send_changes(struct ctrl *c, uint32_t now)
{
	for (int id = 0; id < CTRL_IDS; id++) {
		if (c->last[id] == c->known[id]) {
			continue;
		}
		/* Mute and solo are buttons; volume and balance rate limited */
		if (id >= CTRL_MUTE) {
			send_pulse(c, id, now);
		} else if (after(now, c->due_ms[id])) {
			send_id(c, id, now);
		}
	}
}

/*
 * Hold PLAY + move fader N: balance of track N, relative, as holding track
 * key N does. Meanwhile rui sees the faders where they were before the press
 * (it does not see PLAY, so it does not wait for the supply to recover);
 * once the supply has recovered, they pick up the volumes again.
 */
static void pan_tick(struct ctrl *c, const struct ui_in *in, struct ui_in *rin,
		     bool play, bool play_pressed, uint32_t now)
{
	struct scene *s = &c->rui.st.current;

	if (play && play_pressed) {
		c->pan_used = false;
		c->frozen = true;
		for (int t = 0; t < SP1M_TRACKS; t++) {
			c->pan_on[t] = false;
			c->freeze[t] = c->prev_fader[t];
			c->pan_fader[t] = in->fader[t];
		}
	}
	if (play) {
		for (int t = 0; t < SP1M_TRACKS; t++) {
			const uint16_t f = in->fader[t];

			if (now - c->play_ms < UI_SAG_SETTLE_MS) {
				/* Reading still drops with the supply: re-anchor */
				c->pan_fader[t] = f;
				continue;
			}
			if (!c->pan_on[t] &&
			    abs((int)f - (int)c->pan_fader[t]) >= UI_BAL_ENTER) {
				c->pan_on[t] = true;
				c->pan_used = true;
				c->pan_fader[t] = f;
				c->pan_bal[t] = s->bal[t];
			}
			if (c->pan_on[t]) {
				int nb = c->pan_bal[t] +
					 (int)lroundf((float)((int)f - (int)c->pan_fader[t]) *
						      UI_BAL_PER_FADER);

				s->bal[t] = (int8_t)sp1m_clampi(nb, -BALANCE_MAX, BALANCE_MAX);
			}
		}
		c->thaw_ms = now + UI_SAG_HOLDOFF_MS + 20u;
	}
	if (!c->frozen) {
		return;
	}
	if (!play && after(now, c->thaw_ms)) {
		c->frozen = false;
		for (int t = 0; t < SP1M_TRACKS; t++) {
			pickup_arm(&c->rui.vol_pick[t], s->vol[t], in->fader[t]);
		}
		return;
	}
	memcpy(rin->fader, c->freeze, sizeof(c->freeze));
}

static void enter(struct ctrl *c, struct ui *local, const struct ui_in *in)
{
	const uint32_t now = in->now_ms;
	struct persist p = local->st;

	/* The DAW's side starts where the faders are, centred, nothing muted
	 * or soloed, no scenes; the settings and the master volume carry on
	 */
	for (int t = 0; t < SP1M_TRACKS; t++) {
		p.current.vol[t] = in->fader[t];
		p.current.bal[t] = 0;
	}
	p.current.mute = 0;
	p.current.solo = 0;
	p.slot_valid = 0;
	ui_init(&c->rui, &p, in->fader, now);
	/* ui_init caps it as at power-up; the volume should not drop here */
	c->rui.st.current.master_db = local->st.current.master_db;
	ui_resume(&c->rui, in);
	c->seen = c->rui.st.current;

	for (int t = 0; t < SP1M_TRACKS; t++) {
		c->last[CTRL_VOL + t] = ctrl_vol7(in->fader[t]);
		c->last[CTRL_BAL + t] = ctrl_bal7(0);
		c->last[CTRL_MUTE + t] = 0;
		c->last[CTRL_SOLO + t] = 0;
	}
	/* Only the volumes go out now: balance, mute and solo have no
	 * position on the SP-1, so the DAW's own settings stay until changed
	 */
	memcpy(c->known, c->last, sizeof(c->known));
	memset(c->touched, 0, sizeof(c->touched));
	for (int id = 0; id < CTRL_IDS; id++) {
		c->due_ms[id] = now;
	}

	c->active = true;
	c->frozen = false;
	c->pan_used = false;
	c->usb_prev = in->usb_configured;
	dump(c, now);
	c->anim = ANIM_ENTER;
	c->anim_ms = now;
}

static void leave(struct ctrl *c, struct ui *local, const struct ui_in *in)
{
	const uint32_t now = in->now_ms;
	const struct persist *r = &c->rui.st;

	/* The master volume and the settings as they are now; the mix as it
	 * was, the faders waiting to pick it up
	 */
	ui_set_master_db(local, r->current.master_db, now);
	ui_set_spk_mode(local, (enum spk_mode)r->current.spk_mode, now);
	ui_set_led_mode(local, (enum led_mode)r->led_mode, now);
	ui_resume(local, in);

	c->active = false;
	c->dump_pending = false;
	c->anim = ANIM_LEAVE;
	c->anim_ms = now;
}

static void overlay(struct ctrl *c, struct ui_out *out, uint32_t now)
{
	uint8_t *pl = &out->led[SP1M_TRACKS];

	if (c->anim != ANIM_NONE && now - c->anim_ms < anim_len[c->anim]) {
		const uint32_t step = (now - c->anim_ms) / 100u % 4u;

		for (uint32_t i = 0; i < 4; i++) {
			if (c->anim == ANIM_SENT) {
				pl[i] = 255;
			} else {
				const uint32_t lit = c->anim == ANIM_ENTER ? step : 3u - step;

				pl[i] = i == lit ? 255 : 0;
			}
		}
		return;
	}
	c->anim = ANIM_NONE;

	if (c->active && c->rui.st.led_mode == LED_MODE_METER &&
	    pl[0] < CTRL_LED_MARK) {
		pl[0] = CTRL_LED_MARK;
	}
}

void ctrl_init(struct ctrl *c, uint8_t channel)
{
	memset(c, 0, sizeof(*c));
	c->available = channel <= 15;
	c->channel = channel;
}

struct ui *ctrl_ui(struct ctrl *c, struct ui *local)
{
	return c->active ? &c->rui : local;
}

void ctrl_set_mode(struct ctrl *c, struct ui *local, const struct ui_in *in,
		   bool on)
{
	if (!c->available || on == c->active) {
		return;
	}
	if (on) {
		enter(c, local, in);
	} else {
		leave(c, local, in);
	}
}

void ctrl_request_dump(struct ctrl *c, uint32_t now_ms)
{
	if (c->active) {
		c->dump_pending = true;
		c->dump_ms = now_ms;
	}
}

void ctrl_tick(struct ctrl *c, struct ui *local, const struct ui_in *in,
	       struct ui_out *out)
{
	const uint32_t now = in->now_ms;
	const uint32_t b = in->buttons & ((1u << BTN_COUNT) - 1u);
	const uint32_t pressed = b & ~c->prev_buttons;
	const uint32_t released = c->prev_buttons & ~b;
	bool toggle = false;
	bool play_tap = false;

	/* FUNCTION + PLAY means nothing to the mixer UI, PLAY alone is the
	 * scene layer there and ours in controller mode
	 */
	if (c->prev_buttons == 0 && b != 0) {
		c->chord = 0;
	}
	c->chord |= b;
	if (in->inputs_lost) {
		c->chord |= CHORD_CANCEL;
	}
	if (pressed & B(PLAY)) {
		c->play_ms = now;
		c->play_masked = (b & B(FN)) == 0;
	}
	if ((released & B(PLAY)) && now - c->play_ms <= UI_TAP_MAX_MS) {
		toggle = c->chord == (B(PLAY) | B(FN));
		play_tap = c->chord == B(PLAY);
	}
	c->prev_buttons = b;

	if (toggle) {
		ctrl_set_mode(c, local, in, !c->active);
	}

	if (!c->active) {
		ui_tick(local, in, out);
		overlay(c, out, now);
		memcpy(c->prev_fader, in->fader, sizeof(c->prev_fader));
		return;
	}

	struct ui_in rin = *in;
	const bool play = (b & B(PLAY)) && c->play_masked;

	if (c->play_masked) {
		rin.buttons &= ~B(PLAY);
	}
	take_external(c);
	pan_tick(c, in, &rin, play, (pressed & B(PLAY)) != 0, now);
	ui_tick(&c->rui, &rin, out);
	take_physical(c, now);
	memcpy(c->prev_fader, in->fader, sizeof(c->prev_fader));

	if (play_tap && !c->pan_used) {
		dump(c, now);
		c->anim = ANIM_SENT;
		c->anim_ms = now;
	}
	if (in->usb_configured && !c->usb_prev) {
		c->dump_pending = true;
		c->dump_ms = now + CTRL_DUMP_DELAY_MS;
	}
	c->usb_prev = in->usb_configured;
	if (c->dump_pending && after(now, c->dump_ms)) {
		dump(c, now);
	}
	send_changes(c, now);

	/* The DAW mixes: the host's audio passes at 0 dB, master applied */
	for (int t = 0; t < SP1M_TRACKS; t++) {
		out->mix.track_gain[t] = 1.0f;
		out->mix.balance[t] = 0.0f;
	}
	out->mix.mute_mask = 0;
	out->mix.solo_mask = 0;
	overlay(c, out, now);
}

bool ctrl_filter_in(struct ctrl *c, const struct midi_msg *m, uint32_t now_ms)
{
	const uint8_t kind = m->status & 0xf0u;
	int id = -1;

	if (!c->active || (m->status & 0x0fu) != c->channel) {
		return true;
	}
	if (kind == 0xc0) {
		c->ignored++;
		return false;
	}
	if (kind != 0xb0) {
		return true;
	}

	uint8_t n = m->d1;

	/* An LSB belongs to the control of its MSB */
	if (n >= 52 && n <= 59) {
		n -= 32;
	}
	for (int k = 0; k < 4; k++) {
		if (n >= cc_base[k] && n < cc_base[k] + SP1M_TRACKS) {
			id = k * 4 + (n - cc_base[k]);
		}
	}
	if (id >= 0 && c->touched[id] && now_ms - c->touch_ms[id] < CTRL_ECHO_MS) {
		c->ignored++;
		return false;
	}

	return true;
}

bool ctrl_take(struct ctrl *c, struct midi_msg *m)
{
	if (c->ob_count == 0) {
		return false;
	}
	*m = c->outbox[c->ob_head];
	c->ob_head = (uint8_t)((c->ob_head + 1u) % CTRL_OUTBOX);
	c->ob_count--;

	return true;
}
