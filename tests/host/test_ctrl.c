/*
 * SPDX-License-Identifier: MIT
 */

#include <stdlib.h>
#include <string.h>

#include "test.h"
#include "ctrl.h"
#include "midi_map.h"

#define T(n)	BTN_BIT(BTN_T##n)
#define FN	BTN_BIT(BTN_FN)
#define PLAY	BTN_BIT(BTN_PLAY)
#define VUP	BTN_BIT(BTN_VOL_UP)

static struct ctrl c;
static struct ui u;
static struct ui_in in;
static struct ui_out out;
static struct midi_params p;
static struct sp1m_midi_context mctx;
static struct midi_msg sent[512];
static int nsent;

static void setup(void)
{
	uint16_t f[SP1M_TRACKS] = { 500, 500, 500, 500 };

	memset(&in, 0, sizeof(in));
	memset(&out, 0, sizeof(out));
	memcpy(in.fader, f, sizeof(f));
	in.now_ms = 1000;
	in.usb_configured = true;
	ui_init(&u, NULL, f, in.now_ms);
	ctrl_init(&c, 0);
	sp1m_midi_params_init(&p, 0);
	p.changed = sp1m_midi_changed;
	p.ctx = &mctx;
	mctx.in = &in;
	nsent = 0;
}

/* One control loop tick, as control.c runs it */
static void step(void)
{
	struct midi_msg m;

	in.now_ms += UI_TICK_MS;
	ctrl_tick(&c, &u, &in, &out);
	while (ctrl_take(&c, &m)) {
		if (nsent < (int)(sizeof(sent) / sizeof(sent[0]))) {
			sent[nsent++] = m;
		}
	}
}

static void hold(uint32_t buttons, uint32_t ms)
{
	in.buttons = buttons;
	for (uint32_t t = 0; t < ms; t += UI_TICK_MS) {
		step();
	}
}

static void tap(uint32_t buttons)
{
	hold(buttons, 100);
	hold(0, 50);
}

static void fn_play(void)
{
	hold(FN, 50);
	hold(FN | PLAY, 80);
	hold(FN, 30);
	hold(0, 30);
}

/* A message from the host, applied as midi_in.c does before a tick */
static void rx(uint8_t status, uint8_t d1, uint8_t d2)
{
	struct midi_msg m = { status, d1, d2, 3 };

	mctx.ui = ctrl_ui(&c, &u);
	sp1m_midi_sync(&p, &mctx.ui->st.current);
	if (ctrl_filter_in(&c, &m, in.now_ms)) {
		midi_params_on_msg(&m, MIDI_SRC_USB, &p);
	}
}

static int count(uint8_t cc)
{
	int n = 0;

	for (int i = 0; i < nsent; i++) {
		n += sent[i].status == 0xb0 && sent[i].d1 == cc;
	}

	return n;
}

static int value(uint8_t cc)
{
	int v = -1;

	for (int i = 0; i < nsent; i++) {
		if (sent[i].status == 0xb0 && sent[i].d1 == cc) {
			v = sent[i].d2;
		}
	}

	return v;
}

/* Move a fader to @to in steps of @stepsize, one per tick */
static void slide(int t, int to, int stepsize)
{
	while (in.fader[t] != to) {
		int f = in.fader[t];

		f = abs(to - f) <= stepsize ? to : f + (f < to ? stepsize : -stepsize);
		in.fader[t] = (uint16_t)f;
		step();
	}
}

TEST(test_ctrl_values)
{
	for (int v = 0; v <= 127; v++) {
		CHECK(ctrl_vol7(sp1m_midi_to_vol(midi_widen7((uint8_t)v))) == v);
		CHECK(ctrl_bal7(sp1m_midi_to_balance(midi_widen7((uint8_t)v), 7)) == v);
	}
	CHECK(ctrl_vol7(0) == 0);
	CHECK(ctrl_vol7(FADER_MAX) == 127);
	CHECK(ctrl_bal7(-BALANCE_MAX) == 0);
	CHECK(ctrl_bal7(0) == 64);
	CHECK(ctrl_bal7(BALANCE_MAX) == 127);
}

TEST(test_ctrl_mode_switch)
{
	struct scene before;

	/* Without MIDI (no channel): FUNCTION + PLAY does nothing */
	setup();
	ctrl_init(&c, 16);
	fn_play();
	CHECK(!c.active);
	CHECK(nsent == 0);

	setup();
	u.st.current.mute = 0x8;
	u.st.current.bal[1] = -40;
	before = u.st.current;
	hold(0, 20);
	fn_play();
	CHECK(c.active);
	CHECK(ctrl_ui(&c, &u) == &c.rui);
	/* The volumes go out at once, nothing else */
	CHECK(nsent == 4);
	for (int t = 0; t < SP1M_TRACKS; t++) {
		CHECK(count(20 + t) == 1);
		CHECK(value(20 + t) == ctrl_vol7(500));
	}
	/* The host's audio at 0 dB; the mixer's own state untouched */
	for (int t = 0; t < SP1M_TRACKS; t++) {
		CHECK(out.mix.track_gain[t] == 1.0f);
		CHECK(out.mix.balance[t] == 0.0f);
	}
	CHECK(out.mix.mute_mask == 0);
	CHECK(memcmp(&u.st.current, &before, sizeof(before)) == 0);
	/* The first playback LED marks the mode */
	hold(0, 1000);
	CHECK(out.led[SP1M_TRACKS] >= 40);

	/* PLAY sends the volumes again and opens no scene layer */
	nsent = 0;
	tap(PLAY);
	CHECK(nsent == 4);
	CHECK(!c.rui.scene_layer);
	CHECK(!u.scene_layer);

	/* Back to the mixer: as it was */
	fn_play();
	CHECK(!c.active);
	CHECK(memcmp(&u.st.current, &before, sizeof(before)) == 0);
	hold(0, 1000);
	CHECK(out.mix.mute_mask == 0x8);
	CHECK(out.mix.track_gain[0] == fader_to_gain(500));
	tap(PLAY);
	CHECK(u.scene_layer);
}

TEST(test_ctrl_fader_sends)
{
	setup();
	fn_play();
	nsent = 0;

	/* 300 units in 31 ticks (62 ms): at most one message per 10 ms */
	slide(0, 800, 10);
	hold(0, 50);
	/* Within a step: where a slow fader stops depends on the way it came */
	CHECK(abs(value(20) - ctrl_vol7(800)) <= 1);
	CHECK(count(20) >= 3);
	CHECK(count(20) <= 9);
	CHECK(u.st.current.vol[0] == 500);

	/* A fader resting on a step boundary sends nothing */
	in.fader[1] = 500;
	hold(0, 20);
	nsent = 0;
	for (int i = 0; i < 200; i++) {
		in.fader[1] = (uint16_t)(i & 1 ? 503 : 504);
		step();
	}
	CHECK(count(21) == 0);
	slide(1, 520, 2);
	hold(0, 20);
	CHECK(abs(value(21) - ctrl_vol7(520)) <= 1);
	CHECK(value(21) > ctrl_vol7(500));

	/* Both ends are reached */
	slide(2, 0, 20);
	hold(0, 20);
	CHECK(value(22) == 0);
	slide(2, FADER_MAX, 20);
	hold(0, 20);
	CHECK(value(22) == 127);
}

TEST(test_ctrl_buttons)
{
	float master;

	setup();
	fn_play();
	hold(0, 50);
	nsent = 0;

	tap(T(2));
	CHECK(nsent == 1);
	CHECK(value(103) == 127);
	CHECK(u.st.current.mute == 0);
	CHECK(out.mix.mute_mask == 0);
	/* The track LED shows the DAW's mute */
	CHECK(out.led[1] == 0);
	tap(T(2));
	CHECK(value(103) == 0);

	hold(FN, 50);
	hold(FN | T(3), 80);
	hold(FN, 30);
	hold(0, 30);
	CHECK(value(108) == 127);
	CHECK(u.st.current.solo == 0);

	/* Balance: hold track 1 and move its fader */
	nsent = 0;
	hold(T(1), 100);
	in.buttons = T(1);
	slide(0, 680, 4);
	hold(T(1), 30);
	hold(0, 30);
	CHECK(count(24) >= 1);
	CHECK(value(24) > 64);
	CHECK(count(20) == 0);
	CHECK(u.st.current.bal[0] == 0);
	/* The fader now waits to pick up the volume again */
	CHECK(c.rui.vol_pick[0].pending);
	slide(0, 700, 4);
	CHECK(count(20) == 0);

	/* The master volume stays on the SP-1 */
	master = out.mix.master_gain;
	nsent = 0;
	tap(VUP);
	CHECK(out.mix.master_gain > master);
	CHECK(nsent == 0);
}

TEST(test_ctrl_echo)
{
	uint32_t ignored;

	setup();
	fn_play();
	hold(0, 400);
	nsent = 0;

	/* Just after the fader moved, what comes back for it is its echo,
	 * here an older value a DAW with MIDI thru would return
	 */
	slide(0, 800, 10);
	hold(0, 30);
	rx(0xb0, 20, 70);
	hold(0, 20);
	CHECK(c.rui.st.current.vol[0] != sp1m_midi_to_vol(midi_widen7(70)));
	CHECK(c.ignored == 1);
	CHECK(abs(value(20) - ctrl_vol7(800)) <= 1);

	/* Later, the host's value applies, is not sent back, and the fader
	 * waits for it
	 */
	hold(0, 400);
	nsent = 0;
	rx(0xb0, 20, 30);
	hold(0, 50);
	CHECK(c.rui.st.current.vol[0] == sp1m_midi_to_vol(midi_widen7(30)));
	CHECK(nsent == 0);
	CHECK(c.rui.vol_pick[0].pending);
	slide(0, 900, 10);
	CHECK(count(20) == 0);
	/* Down through the host's value: the fader has it again */
	slide(0, 100, 10);
	hold(0, 50);
	CHECK(count(20) >= 1);
	CHECK(abs(value(20) - ctrl_vol7(100)) <= 1);

	/* A mute echo is dropped; a later change from the host is not sent back */
	hold(0, 400);
	nsent = 0;
	tap(T(1));
	CHECK(value(102) == 127);
	rx(0xb0, 102, 127);
	hold(0, 400);
	rx(0xb0, 102, 0);
	hold(0, 20);
	CHECK((c.rui.st.current.mute & 1) == 0);
	CHECK(count(102) == 1);

	/* No scenes for the DAW */
	ignored = c.ignored;
	rx(0xc0, 0, 0);
	CHECK(c.ignored == ignored + 1);
}

TEST(test_ctrl_usb_dump)
{
	setup();
	in.usb_configured = false;
	fn_play();
	CHECK(nsent == 4);
	nsent = 0;
	in.usb_configured = true;
	hold(0, 100);
	CHECK(nsent == 0);
	hold(0, CTRL_DUMP_DELAY_MS);
	CHECK(nsent == 4);
}

TEST(test_ctrl_mixer_unchanged)
{
	static struct ui u2;
	struct ui_out out2;
	uint16_t f[SP1M_TRACKS] = { 500, 500, 500, 500 };
	const uint32_t seq[] = { 0, T(2), 0, FN, FN | T(3), 0, T(1), 0, VUP, 0,
				 PLAY, 0, T(4), 0, PLAY, 0, BTN_BIT(BTN_FWD), 0 };
	bool same = true;

	setup();
	ui_init(&u2, NULL, f, in.now_ms);
	memset(&out2, 0, sizeof(out2));
	for (size_t i = 0; i < sizeof(seq) / sizeof(seq[0]); i++) {
		for (int k = 0; k < 40; k++) {
			in.buttons = seq[i];
			in.fader[0] = (uint16_t)(400 + 3 * k + 7 * (int)i);
			in.now_ms += UI_TICK_MS;
			ctrl_tick(&c, &u, &in, &out);
			ui_tick(&u2, &in, &out2);
			same = same && memcmp(&out, &out2, sizeof(out)) == 0 &&
			       memcmp(&u.st, &u2.st, sizeof(u.st)) == 0;
		}
	}
	CHECK(same);
	CHECK(!c.active);
	CHECK(c.sent == 0);
}
