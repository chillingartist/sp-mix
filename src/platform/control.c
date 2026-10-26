/*
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/spinlock.h>
#include <zephyr/sys/reboot.h>

#include "board_io.h"
#include "buttons.h"
#include "control.h"
#include "fader.h"
#include "inputs.h"
#include "leds.h"
#include "safety.h"
#include "stall.h"
#include "store.h"
#include "ui.h"

#if defined(CONFIG_SP1M_AUDIO_OUT)
#include "audio.h"
#include "codec.h"
#endif
#if defined(CONFIG_SP1M_USB)
#include "usb.h"
#endif
#if defined(CONFIG_SP1M_MIDI)
#include "ctrl.h"
#include "midi_in.h"
#endif

#define FADER_ALPHA		0.25f
#define DEBOUNCE_SAMPLES	2
#define HP_POLL_MS		50
#define HP_DEBOUNCE		3
#define STANDBY_ON_HOLD_MS	600
/* No good ADC scan for this long: the ladder buttons count as released */
#define SCAN_LOST_MS		40
/* ... and after this long: reset (not again if that did not help) */
#define SCAN_FAIL_RESET_MS	1000

K_MSGQ_DEFINE(ctrl_q, sizeof(struct ctrl_cmd), 8, 4);

static struct ui ui;
#if defined(CONFIG_SP1M_MIDI)
/* Controller mode; the controls drive ctrl_ui(): the mixer or the DAW's side */
static struct ctrl ctrl;
#define ACTIVE_UI()	ctrl_ui(&ctrl, &ui)
#else
#define ACTIVE_UI()	(&ui)
#endif
static struct fader faders[SP1M_TRACKS];
static struct btn_debounce debounce;
static struct ui_in in;
static struct ui_out out;
static struct raw_inputs raw;

static uint32_t last_scan_ms;
static bool scan_ok_this_boot;
static uint8_t busy_run;		/* raw ladder scans in a row not idle */

static bool hp_present;
static bool hp_known;		/* jack sense read at least once */
static bool speaker_on;		/* the amplifier really is in this state */
static bool speaker_forced;
#if defined(CONFIG_SP1M_AUDIO_OUT)
static bool hp_raw;
static uint8_t hp_count;
static uint32_t hp_failures;	/* jack reads failed in a row */
static uint32_t speaker_retry_ms;
#endif
static uint32_t store_writes;
static uint32_t store_errors;

static struct k_spinlock status_lock;
static struct ctrl_status status;

int control_post(const struct ctrl_cmd *cmd)
{
	return k_msgq_put(&ctrl_q, cmd, K_NO_WAIT);
}

void control_get_status(struct ctrl_status *dst)
{
	k_spinlock_key_t key = k_spin_lock(&status_lock);

	*dst = status;
	k_spin_unlock(&status_lock, key);
}

static void publish_status(void)
{
	const struct ui *a = ACTIVE_UI();
	k_spinlock_key_t key = k_spin_lock(&status_lock);

	status.raw = raw;
	status.buttons = in.buttons;
	memcpy(status.fader, in.fader, sizeof(status.fader));
	status.cur = a->st.current;
	status.slot_valid = ui.st.slot_valid;
	status.scene_layer = a->scene_layer;
	status.ctrl_mode = a != &ui;
	status.hp_present = hp_present;
	status.hp_known = hp_known;
	status.speaker_on = speaker_on;
	status.speaker_forced = speaker_forced;
	status.charging = in.charging;
	status.charge_off = ui.st.charge_off != 0;
	status.dirty = ui.dirty;
	status.store_writes = store_writes;
	status.store_errors = store_errors;
	for (int t = 0; t < SP1M_TRACKS; t++) {
		status.pickup_pending[t] = a->vol_pick[t].pending;
	}
	k_spin_unlock(&status_lock, key);
}

static void quiesce(void);

/*
 * The SAADC gave nothing usable for a while. Buttons must not freeze: the
 * ladder ones count as released (the UI is told, so that is no tap) and
 * FUNCTION, which has its own pin, keeps working. If it lasts, reset once;
 * not again when the previous reset was for this and no scan worked since.
 */
static void inputs_lost(uint32_t now)
{
	struct safety_fault f;

	in.inputs_lost = true;
	in.buttons = btn_debounce_update(&debounce, raw.fn ? BTN_BIT(BTN_FN) : 0,
					 DEBOUNCE_SAMPLES);

	safety_last_fault(&f);
	if (now - last_scan_ms > SCAN_FAIL_RESET_MS &&
	    (f.culprit != SAFETY_CULPRIT_INPUTS || scan_ok_this_boot)) {
		quiesce();
		safety_reset(SAFETY_CULPRIT_INPUTS);
	}
}

static void read_inputs(void)
{
	int err = inputs_scan(&raw);
	uint32_t now = k_uptime_get_32();
	uint32_t mask;

	if (err != 0) {
		/* Still converting, or a failed scan the driver retries:
		 * keep the last state until it has been too long
		 */
		if (now - last_scan_ms > SCAN_LOST_MS) {
			inputs_lost(now);
		}
		return;
	}

	last_scan_ms = now;
	scan_ok_this_boot = true;
	in.inputs_lost = false;

	mask = buttons_from_ladders(raw.ladder1, raw.ladder2);
	/* Pressed or between levels: either way current flows. Two scans in
	 * a row, so that a single noisy sample does not hold the faders; the
	 * fader median filter lets a sag through on the second scan too
	 */
	busy_run = mask != 0 ? MIN(busy_run + 1u, 2u) : 0u;
	in.ladder_busy = busy_run >= 2u;
	if (mask != BTN_INVALID && raw.fn) {
		mask |= BTN_BIT(BTN_FN);
	}
	for (int t = 0; t < SP1M_TRACKS; t++) {
		in.fader[t] = fader_update(&faders[t], raw.fader[t], FADER_ALPHA);
	}

	in.buttons = btn_debounce_update(&debounce, mask, DEBOUNCE_SAMPLES);
}

static void flush_persist(void)
{
	struct persist snap;
	uint32_t gen;

	if (!ui.dirty) {
		return;
	}
	ui_persist_snapshot(&ui, &snap, &gen);
	if (store_save_now(&snap) == 0) {
		ui_persist_done(&ui, gen);
		store_writes++;
	} else {
		store_errors++;
	}
}

static void wait_fn_release(void)
{
	while (board_fn_pressed()) {
		safety_wdt_feed();
		stall_tick();
		k_msleep(20);
	}
	k_msleep(60);
}

/* Silence and stop everything that makes sound or talks to the host. */
static void quiesce(void)
{
	stall_stage(STALL_STAGE_FLUSH);
	safety_expect(SAFETY_HB_AUDIO, false);
#if defined(CONFIG_SP1M_AUDIO_OUT)
	audio_shutdown();
#endif
	inputs_shutdown();
	safety_wdt_feed();
	flush_persist();
	safety_wdt_feed();
}

static FUNC_NORETURN void enter_bootloader(void)
{
	static const uint8_t cue[LEDS_COUNT] = { 255, 255, 255, 255, 0, 0, 0, 0 };

	leds_set(cue);
	quiesce();
	/* The user still holds Track 1 + 4: the bootloader sees them. */
	safety_enter_bootloader();
}

/*
 * VBUS present: SYSTEM_OFF would never wake up again on USB. Show the charge
 * state until FUNCTION is held (reboot into the mixer) or USB goes away.
 */
static void standby(void)
{
	uint32_t held = 0;

	stall_stage(STALL_STAGE_STANDBY);
	wait_fn_release();
	board_btn_supply(false);

	while (board_usb_power()) {
		uint8_t l[LEDS_COUNT] = { 0 };
		uint32_t now = k_uptime_get_32();

		safety_wdt_feed();
		stall_tick();
		if (board_charging()) {
			l[LEDS_COUNT - 1] = (now / 1000) & 1 ? 120 : 20;
		} else {
			l[LEDS_COUNT - 1] = 40;
		}
		leds_set(l);

		if (board_fn_pressed()) {
			held += 20;
			if (held >= STANDBY_ON_HOLD_MS) {
				leds_all(0);
				sys_reboot(SYS_REBOOT_WARM);
			}
		} else {
			held = 0;
		}
		k_msleep(20);
	}
}

static FUNC_NORETURN void power_off(void)
{
	stall_stage(STALL_STAGE_OFF);
	/* Playback LEDs sweep out */
	for (int i = LEDS_COUNT - 1; i >= 0; i--) {
		uint8_t l[LEDS_COUNT] = { 0 };

		for (int j = 0; j < i; j++) {
			l[j] = 120;
		}
		leds_set(l);
		safety_wdt_feed();
		stall_tick();
		k_msleep(40);
	}

	quiesce();
	stall_stage(STALL_STAGE_OFF);
#if defined(CONFIG_SP1M_USB)
	usb_shutdown();
#endif

	if (board_usb_power()) {
		standby();
	}

	stall_stage(STALL_STAGE_OFF);
	wait_fn_release();
	leds_all(0);
	board_prepare_system_off();
	safety_system_off();
}

static void handle_cmd(const struct ctrl_cmd *c)
{
	switch (c->type) {
	case CTRL_SCENE_SAVE:
		(void)ui_scene_save(&ui, c->arg, in.now_ms);
		break;
	case CTRL_SCENE_LOAD:
		(void)ui_scene_recall(&ui, c->arg, in.fader, in.now_ms);
		break;
	case CTRL_SPEAKER:
		speaker_forced = false;
		ui_set_spk_mode(ACTIVE_UI(), (enum spk_mode)c->arg, in.now_ms);
		break;
	case CTRL_SPEAKER_FORCE:
		speaker_forced = c->arg != 0;
		break;
	case CTRL_MASTER:
		ui_set_master_db(ACTIVE_UI(), c->arg, in.now_ms);
		break;
#if defined(CONFIG_SP1M_MIDI)
	case CTRL_MODE:
		ctrl_set_mode(&ctrl, &ui, &in, c->arg != 0);
		break;
	case CTRL_MIDI_DUMP:
		ctrl_request_dump(&ctrl, in.now_ms);
		break;
#endif
	case CTRL_REBOOT:
		/* Mute and power the codecs down properly, then reboot */
		quiesce();
		sys_reboot(SYS_REBOOT_WARM);
		break;
	case CTRL_CHARGE:
		ui_set_charge_off(&ui, c->arg == 0, in.now_ms);
		board_charger_enable(c->arg != 0);
		break;
	case CTRL_BOOTLOADER:
		enter_bootloader();
		break;
	case CTRL_POWER_OFF:
		power_off();
		break;
	case CTRL_SELFTEST:
		/* Silence first: these resets must not catch the codecs playing */
		quiesce();
		if (c->arg == 0) {
			/* The stall catcher resets after 1.5 s (stage CMDS) */
			stall_stage(STALL_STAGE_CMDS);
			k_msleep(3000);
		} else {
			/* The loop keeps running; the watchdog resets */
			safety_selftest_wdt();
		}
		break;
	default:
		break;
	}
}

static bool writes_allowed(void)
{
#if defined(CONFIG_SP1M_UAC2)
	/* Erasing flash stalls the CPU: never while the host plays. */
	return !usb_streaming();
#else
	return true;
#endif
}

static void persistence(void)
{
	uint32_t gen;
	bool ok;

	if (store_take_done(&gen, &ok)) {
		if (ok) {
			ui_persist_done(&ui, gen);
			store_writes++;
		} else {
			store_errors++;
		}
	}

	if (!store_busy() && writes_allowed() && ui_persist_due(&ui, in.now_ms)) {
		struct persist snap;

		ui_persist_snapshot(&ui, &snap, &gen);
		store_request(&snap, gen);
	}
}

#if defined(CONFIG_SP1M_AUDIO_OUT)
/*
 * Before the loop: decide on the jack by majority, so that the speaker does
 * not come on for the debounce time with headphones plugged in.
 */
static void hp_boot_vote(void)
{
	int reads = 0, yes = 0;

	for (int i = 0; i < 5; i++) {
		bool p;

		if (codec_headphones_read(&p) == 0) {
			reads++;
			yes += p ? 1 : 0;
		}
		k_msleep(2);
	}
	if (reads > 0) {
		/* A tie counts as plugged in: speaker off */
		hp_present = 2 * yes >= reads;
		hp_raw = hp_present;
		hp_count = HP_DEBOUNCE;
		hp_known = true;
	}
}
#endif

static void poll_outputs(uint32_t now)
{
#if defined(CONFIG_SP1M_AUDIO_OUT)
	static uint32_t next_hp;
	static uint32_t last_underruns;
	struct audio_levels lv;
	struct audio_stats st;

	if ((int32_t)(now - next_hp) >= 0) {
		bool p;

		if (codec_headphones_read(&p) != 0) {
			/* Keep the last state; poll less often while it fails */
			next_hp = now + (++hp_failures >= 3 ? 1000u : HP_POLL_MS);
		} else {
			hp_failures = 0;
			next_hp = now + HP_POLL_MS;
			if (p == hp_raw) {
				if (hp_count < HP_DEBOUNCE) {
					hp_count++;
				}
			} else {
				hp_raw = p;
				hp_count = 1;
			}
			if (hp_count >= HP_DEBOUNCE) {
				hp_present = hp_raw;
				hp_known = true;
			}
		}
	}

	in.out_peak = audio_get_levels(&lv) ? lv.out : 0.0f;

	audio_get_stats(&st);
	in.underrun = st.underruns != last_underruns;
	last_underruns = st.underruns;
#else
	ARG_UNUSED(now);
#endif
#if defined(CONFIG_SP1M_UAC2)
	/* Every packet as received: the same samples the mixer gets */
	usb_take_track_peaks(in.track_peak);
#endif
}

FUNC_NORETURN void control_run(const struct persist *loaded)
{
	struct k_timer tick;
	struct ctrl_cmd cmd;
	uint16_t f0[SP1M_TRACKS];

	stall_start();
	stall_stage(STALL_STAGE_SETTLE);
	last_scan_ms = k_uptime_get_32();

	for (int t = 0; t < SP1M_TRACKS; t++) {
		fader_init(&faders[t]);
	}
	/* Let the median and low-pass filters settle on the real positions */
	for (int i = 0; i < 20; i++) {
		read_inputs();
		stall_tick();
		k_msleep(UI_TICK_MS);
	}
	memcpy(f0, in.fader, sizeof(f0));

	ui_init(&ui, loaded, f0, k_uptime_get_32());
	board_charger_enable(ui.st.charge_off == 0);
#if defined(CONFIG_SP1M_MIDI)
	ctrl_init(&ctrl, CONFIG_SP1M_MIDI_CHANNEL - 1);
	midi_in_start(&ui, &in, &ctrl);
#endif

#if defined(CONFIG_SP1M_AUDIO_OUT)
	hp_boot_vote();
	/* Only a running audio thread beats; without it this would end in a
	 * watchdog reset loop
	 */
	if (audio_running()) {
		safety_expect(SAFETY_HB_AUDIO, true);
	}
#endif

	k_timer_init(&tick, NULL, NULL);
	k_timer_start(&tick, K_MSEC(UI_TICK_MS), K_MSEC(UI_TICK_MS));

	for (;;) {
		stall_stage(STALL_STAGE_WAIT);
		k_timer_status_sync(&tick);
		stall_tick();
		in.now_ms = k_uptime_get_32();

		stall_stage(STALL_STAGE_INPUTS);
		read_inputs();
		stall_stage(STALL_STAGE_OUTPUTS);
		poll_outputs(in.now_ms);
		in.hp_present = hp_present;
#if defined(CONFIG_SP1M_AUDIO_OUT)
		/* A jack that cannot be read may have headphones in it */
		in.hp_maybe = hp_raw || hp_failures >= 3u;
#endif
		in.charging = board_charging();
#if defined(CONFIG_SP1M_USB)
		in.usb_configured = usb_configured();
		in.streaming = usb_streaming();
#endif

		stall_stage(STALL_STAGE_UI);
#if defined(CONFIG_SP1M_MIDI)
		midi_in_poll();
		ctrl_tick(&ctrl, &ui, &in, &out);
		midi_out_flush();
#else
		ui_tick(&ui, &in, &out);
#endif

		stall_stage(STALL_STAGE_MIX);
#if defined(CONFIG_SP1M_AUDIO_OUT)
		audio_set_params(&out.mix);
		{
			/* Off until the jack has been read, unless forced. If
			 * the CS42L42 never came up, the headphones are muted
			 * and the jack cannot be read: speaker only
			 */
			bool jack_ok = codec_jack_usable();
			bool want = speaker_forced ||
				    ((hp_known || !jack_ok) && out.speaker_on);

			if (want != speaker_on &&
			    (int32_t)(in.now_ms - speaker_retry_ms) >= 0) {
				if (codec_speaker_enable(want)) {
					speaker_on = want;
				} else {
					speaker_retry_ms = in.now_ms + 100u;
				}
			}
		}
#else
		speaker_on = out.speaker_on;
#endif
		stall_stage(STALL_STAGE_LEDS);
		leds_set(out.led);

		stall_stage(STALL_STAGE_CMDS);
		while (k_msgq_get(&ctrl_q, &cmd, K_NO_WAIT) == 0) {
			handle_cmd(&cmd);
		}

		if (ui_take_dfu(ACTIVE_UI())) {
			enter_bootloader();
		}
		if (ui_take_power_off(ACTIVE_UI())) {
			power_off();
		}

		stall_stage(STALL_STAGE_PERSIST);
		persistence();
		stall_stage(STALL_STAGE_STATUS);
		publish_status();
		safety_supervise(in.now_ms);
	}
}
