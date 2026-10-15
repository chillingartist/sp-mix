/*
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

#include "test.h"
#include "ui.h"

#define T(n) BTN_BIT(BTN_T##n)
#define FN BTN_BIT(BTN_FN)

static struct ui u;
static struct ui_in in;
static struct ui_out out;

static void setup(void)
{
	uint16_t f[SP1M_TRACKS] = { 500, 500, 500, 500 };

	memset(&in, 0, sizeof(in));
	memset(&out, 0, sizeof(out));
	memcpy(in.fader, f, sizeof(f));
	in.now_ms = 1000;
	in.usb_configured = true;
	ui_init(&u, NULL, f, in.now_ms);
}

/* Run for @ms with the given buttons held */
static void hold(uint32_t buttons, uint32_t ms)
{
	in.buttons = buttons;
	for (uint32_t t = 0; t < ms; t += UI_TICK_MS) {
		in.now_ms += UI_TICK_MS;
		ui_tick(&u, &in, &out);
	}
}

static void tap(uint32_t buttons)
{
	hold(buttons, 100);
	hold(0, 50);
}

TEST(test_ui_mute_solo)
{
	setup();
	hold(0, 10);
	CHECK(out.mix.track_gain[1] > 0.0f);

	tap(T(2));
	CHECK(u.st.current.mute == 0x2);
	CHECK(out.mix.mute_mask == 0x2);
	tap(T(2));
	CHECK(u.st.current.mute == 0);

	/* FUNCTION + track = solo, also when FUNCTION is let go first */
	hold(FN, 50);
	hold(FN | T(3), 80);
	hold(T(3), 30);
	hold(0, 30);
	CHECK(u.st.current.solo == 0x4);
	CHECK(u.st.current.mute == 0);

	/* A long press is not a tap */
	hold(T(1), UI_TAP_MAX_MS + 50);
	hold(0, 20);
	CHECK(u.st.current.mute == 0);

	/* Two track keys together: no toggles */
	hold(T(1), 20);
	hold(T(1) | T(2), 50);
	hold(T(2), 20);
	hold(0, 20);
	CHECK(u.st.current.mute == 0);
}

TEST(test_ui_balance)
{
	setup();
	hold(0, 10);
	CHECK(u.st.current.vol[0] == 500);

	/* Hold Track 1 and push its fader up: balance moves right */
	hold(T(1), 50);
	for (int f = 500; f <= 800; f += 10) {
		in.fader[0] = (uint16_t)f;
		hold(T(1), 4);
	}
	CHECK(u.st.current.bal[0] > 80);
	CHECK(u.st.current.vol[0] == 500);	/* volume untouched */
	hold(0, 20);
	CHECK(u.st.current.mute == 0);		/* no mute toggle */

	/* Volume needs a pickup now: moving above 500 does nothing */
	CHECK(u.vol_pick[0].pending);
	in.fader[0] = 700;
	hold(0, 20);
	CHECK(u.st.current.vol[0] == 500);
	in.fader[0] = 495;			/* crosses the stored value */
	hold(0, 20);
	CHECK(!u.vol_pick[0].pending);
	in.fader[0] = 450;
	hold(0, 20);
	CHECK(u.st.current.vol[0] == 450);

	/* Balance from the left side is clamped */
	hold(T(1), 20);
	for (int f = 450; f >= 0; f -= 10) {
		in.fader[0] = (uint16_t)f;
		hold(T(1), 4);
	}
	hold(0, 10);
	CHECK(u.st.current.bal[0] >= -BALANCE_MAX);
	CHECK(u.st.current.bal[0] < u.hold_bal[0]);
}

/* Buttons forced up because the ADC stopped answering are not taps */
TEST(test_ui_inputs_lost)
{
	setup();
	hold(0, 10);

	hold(T(2), 100);
	in.inputs_lost = true;
	hold(0, 50);
	CHECK(u.st.current.mute == 0);

	hold(BTN_BIT(BTN_PLAY), 100);
	hold(0, 50);
	CHECK(!u.scene_layer);

	/* Inputs back: a fresh press taps again */
	in.inputs_lost = false;
	hold(0, 10);
	tap(T(2));
	CHECK(u.st.current.mute == 0x2);
	tap(BTN_BIT(BTN_PLAY));
	CHECK(u.scene_layer);
}

TEST(test_ui_supply_sag)
{
	setup();
	hold(0, 100);
	CHECK(u.st.current.vol[2] == 500);

	/* Pressing a volume key drags every fader reading down 4 % */
	hold(BTN_BIT(BTN_VOL_UP), 6);
	for (int t = 0; t < SP1M_TRACKS; t++) {
		in.fader[t] = 480;
	}
	hold(BTN_BIT(BTN_VOL_UP), 200);
	CHECK(u.st.current.vol[2] == 500);
	/* Released: the supply recovers within the holdoff */
	for (int t = 0; t < SP1M_TRACKS; t++) {
		in.fader[t] = 500;
	}
	hold(0, 100);
	CHECK(u.st.current.vol[0] == 500 && u.st.current.vol[2] == 500);

	/* A track tap with the same sag is still a mute, not a balance move */
	hold(T(2), 4);
	for (int t = 0; t < SP1M_TRACKS; t++) {
		in.fader[t] = 476;
	}
	hold(T(2), 120);
	for (int t = 0; t < SP1M_TRACKS; t++) {
		in.fader[t] = 500;
	}
	hold(0, 100);
	CHECK(u.st.current.mute == 0x2);
	CHECK(u.st.current.bal[1] == 0);
	CHECK(u.st.current.vol[1] == 500);
	CHECK(!u.vol_pick[1].pending);

	/* Real fader moves still work once the button is released */
	in.fader[2] = 700;
	hold(0, 20);
	CHECK(u.st.current.vol[2] == 700);
}

TEST(test_ui_sag_before_debounce)
{
	struct persist p;
	uint16_t f[SP1M_TRACKS] = { 900, 900, 900, 900 };

	/* Faders below their stored values: pickups pending */
	persist_default(&p);
	for (int t = 0; t < SP1M_TRACKS; t++) {
		p.current.vol[t] = 1023;
	}
	memset(&in, 0, sizeof(in));
	memcpy(in.fader, f, sizeof(f));
	in.now_ms = 1000;
	ui_init(&u, &p, f, in.now_ms);
	hold(0, 100);
	CHECK(u.vol_pick[0].pending);

	/* The scan that first shows the press already shows a 6 % sag; the
	 * debounced mask follows several scans later
	 */
	in.ladder_busy = true;
	for (int t = 0; t < SP1M_TRACKS; t++) {
		in.fader[t] = 846;
	}
	hold(0, 20);
	hold(BTN_BIT(BTN_VOL_UP), 100);
	hold(0, 6);
	in.ladder_busy = false;
	hold(0, 10);
	for (int t = 0; t < SP1M_TRACKS; t++) {
		in.fader[t] = 900;
	}
	hold(0, 100);
	for (int t = 0; t < SP1M_TRACKS; t++) {
		CHECK(u.vol_pick[t].pending);
		CHECK(u.st.current.vol[t] == 1023);
	}

	/* A real pull-down still takes over */
	in.fader[0] = 846;
	hold(0, 20);
	CHECK(!u.vol_pick[0].pending);
	CHECK(u.st.current.vol[0] == 846);

	/* A raw reading stuck between levels does not freeze the faders */
	in.ladder_busy = true;
	in.fader[1] = 700;
	hold(0, UI_RAW_BUSY_MAX_MS + UI_SAG_HOLDOFF_MS + 20);
	CHECK(u.st.current.vol[1] == 700);
	in.ladder_busy = false;
}

TEST(test_ui_recovery_and_power)
{
	setup();

	/* Track 1 + 4: bootloader only after 3 s */
	hold(T(1) | T(4), UI_HOLD_DFU_MS - 100);
	CHECK(!ui_take_dfu(&u));
	CHECK(out.led[0] > 200);		/* countdown on the track LEDs */
	hold(T(1) | T(4), 200);
	CHECK(ui_take_dfu(&u));
	CHECK(!ui_take_dfu(&u));		/* latched once */
	hold(T(4), 30);
	hold(0, 30);
	CHECK(u.st.current.mute == 0);		/* release sweep is no tap */

	/* Even inside the scene layer */
	tap(BTN_BIT(BTN_PLAY));
	CHECK(u.scene_layer);
	hold(T(1) | T(4), UI_HOLD_DFU_MS + 10);
	CHECK(ui_take_dfu(&u));
	hold(0, 10);

	/* FUNCTION alone for 3 s powers off */
	hold(FN, UI_HOLD_OFF_MS - 50);
	CHECK(!ui_take_power_off(&u));
	hold(FN, 100);
	CHECK(ui_take_power_off(&u));
	hold(0, 10);

	/* FUNCTION used as a modifier never powers off */
	hold(FN, 500);
	hold(FN | T(2), 100);
	hold(FN, UI_HOLD_OFF_MS);
	CHECK(!ui_take_power_off(&u));
	hold(0, 10);

	/* Auto power off without a host */
	in.usb_configured = false;
	hold(0, 1000);
	CHECK(!ui_take_power_off(&u));
	in.now_ms += UI_AUTO_OFF_MS;
	hold(0, 10);
	CHECK(ui_take_power_off(&u));
}

TEST(test_ui_scenes)
{
	setup();
	u.st.current.mute = 0x1;

	/* PLAY, then FUNCTION + Track 2 saves slot 1 */
	tap(BTN_BIT(BTN_PLAY));
	CHECK(u.scene_layer);
	hold(FN, 20);
	hold(FN | T(2), 80);
	hold(0, 20);
	CHECK(u.st.slot_valid == 0x2);
	CHECK(u.st.slot[1].mute == 0x1);
	CHECK(u.scene_layer);

	/* Layer times out */
	hold(0, UI_SCENE_LAYER_MS + 50);
	CHECK(!u.scene_layer);

	/* Change things, then recall */
	tap(T(1));
	CHECK(u.st.current.mute == 0);
	in.fader[2] = 900;
	hold(0, 20);
	CHECK(u.st.current.vol[2] == 900);

	tap(BTN_BIT(BTN_PLAY));
	tap(T(2));
	CHECK(u.st.current.mute == 0x1);
	CHECK(u.st.current.vol[2] == 500);
	CHECK(u.vol_pick[2].pending);		/* fader is at 900 */
	CHECK(!u.vol_pick[0].pending);

	/* Empty slot cannot be recalled */
	CHECK(!ui_scene_recall(&u, 3, in.fader, in.now_ms));
	CHECK(!ui_scene_save(&u, 4, in.now_ms));
}

/* The master volume never comes up loud by surprise */
TEST(test_ui_volume_safety)
{
	struct persist p;
	uint16_t f[SP1M_TRACKS] = { 0, 0, 0, 0 };

	/* Stored loud state, faders at the bottom */
	persist_default(&p);
	p.current.master_db = MASTER_DB_MAX;
	p.current.vol[0] = 1023;
	memset(&in, 0, sizeof(in));
	memcpy(in.fader, f, sizeof(f));
	in.now_ms = 1000;
	ui_init(&u, &p, f, in.now_ms);
	hold(0, 10);
	CHECK(u.st.current.master_db == UI_MASTER_SAFE_DB);

	/* Pulling the fader down takes over even below the stored value */
	CHECK(out.mix.track_gain[0] > 0.9f);
	in.fader[0] = 300;
	hold(0, 50);
	CHECK(u.st.current.vol[0] == 1023);
	in.fader[0] = 250;
	hold(0, 50);
	CHECK(u.st.current.vol[0] == 250);
	in.fader[0] = 0;
	hold(0, 50);
	CHECK(out.mix.track_gain[0] == 0.0f);

	/* Plugging in headphones brings a loud master down at the first
	 * reading, before the debounce switches the speaker off
	 */
	ui_set_master_db(&u, -6, in.now_ms);
	hold(0, 10);
	CHECK(out.speaker_on);
	in.hp_maybe = true;
	hold(0, UI_TICK_MS);
	CHECK(u.st.current.master_db == UI_MASTER_SAFE_DB);
	CHECK(out.speaker_on);
	/* A reading that does not hold up leaves the cap alone */
	in.hp_maybe = false;
	hold(0, 10);
	CHECK(u.st.current.master_db == UI_MASTER_SAFE_DB);
	ui_set_master_db(&u, -6, in.now_ms);
	hold(0, 10);
	CHECK(u.st.current.master_db == -6);
	in.hp_present = true;
	hold(0, 10);
	CHECK(u.st.current.master_db == UI_MASTER_SAFE_DB);
	CHECK(!out.speaker_on);
	/* Debounced and raw both still say plugged in: no second cap */
	in.hp_maybe = true;
	ui_set_master_db(&u, -10, in.now_ms);
	hold(0, 10);
	CHECK(u.st.current.master_db == -10);	/* raising it again is up to you */

	/* Recalling a scene never raises the master */
	CHECK(ui_scene_save(&u, 0, in.now_ms));
	ui_set_master_db(&u, -40, in.now_ms);
	CHECK(ui_scene_recall(&u, 0, in.fader, in.now_ms));
	CHECK(u.st.current.master_db == -40);
	ui_set_master_db(&u, -30, in.now_ms);
	CHECK(ui_scene_save(&u, 1, in.now_ms));
	ui_set_master_db(&u, -12, in.now_ms);
	CHECK(ui_scene_recall(&u, 1, in.fader, in.now_ms));
	CHECK(u.st.current.master_db == -30);
}

TEST(test_ui_master_volume)
{
	setup();
	int8_t db0 = u.st.current.master_db;

	tap(BTN_BIT(BTN_VOL_UP));
	CHECK(u.st.current.master_db == db0 + 1);

	/* Held: repeats after the delay */
	hold(BTN_BIT(BTN_VOL_DOWN), UI_VOL_REPEAT_DELAY_MS + 10 * UI_VOL_REPEAT_MS - 1);
	hold(0, 10);
	CHECK(u.st.current.master_db == db0 + 1 - 1 - 10);

	/* Down to off and back */
	hold(BTN_BIT(BTN_VOL_DOWN), 10000);
	hold(0, 10);
	CHECK(u.st.current.master_db == MASTER_DB_OFF);
	CHECK(out.mix.master_gain == 0.0f);
	tap(BTN_BIT(BTN_VOL_UP));
	CHECK(u.st.current.master_db == MASTER_DB_MIN);

	/* Speaker mode and LED mode */
	CHECK(out.speaker_on);
	in.hp_present = true;
	hold(0, 10);
	CHECK(!out.speaker_on);
	in.hp_present = false;
	hold(FN, 20);
	hold(FN | BTN_BIT(BTN_FWD), 50);
	hold(0, 20);
	CHECK(u.st.current.spk_mode == SPK_OFF);
	CHECK(!out.speaker_on);
	tap(BTN_BIT(BTN_BACK));
	CHECK(u.st.led_mode == LED_MODE_STATUS);
}

TEST(test_ui_persist)
{
	uint32_t gen;
	struct persist snap;

	setup();
	hold(0, 10);
	CHECK(!ui_persist_due(&u, in.now_ms));

	/* Fader moves are written once things are quiet */
	in.fader[1] = 600;
	hold(0, 10);
	CHECK(!ui_persist_due(&u, in.now_ms));
	hold(0, UI_PERSIST_QUIET_MS);
	CHECK(ui_persist_due(&u, in.now_ms));

	ui_persist_snapshot(&u, &snap, &gen);
	CHECK(snap.current.vol[1] == 600);

	/* A change while writing keeps the state dirty */
	in.fader[1] = 650;
	hold(0, 10);
	ui_persist_done(&u, gen);
	CHECK(u.dirty);
	ui_persist_snapshot(&u, &snap, &gen);
	ui_persist_done(&u, gen);
	CHECK(!u.dirty);

	/* Scene saves are due immediately */
	CHECK(ui_scene_save(&u, 0, in.now_ms));
	CHECK(ui_persist_due(&u, in.now_ms));

	/* Restoring arms pickups where faders disagree */
	struct ui u2;
	uint16_t f[SP1M_TRACKS] = { 0, 650, 1023, 500 };

	ui_init(&u2, &snap, f, 0);
	CHECK(u2.vol_pick[0].pending && !u2.vol_pick[1].pending);
	CHECK(u2.st.current.vol[1] == 650);
}
