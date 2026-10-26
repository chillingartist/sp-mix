/*
 * SPDX-License-Identifier: MIT
 *
 * Interaction state machine: buttons and faders in, mix parameters, LED
 * brightness and requests (power off, bootloader) out. Called from the
 * control thread every UI_TICK_MS; nothing in here blocks or touches hardware.
 *
 * Mapping (docs/DESIGN.md section 6):
 *   fader N                      track N volume (pickup after recall/boot)
 *   tap track N                  mute toggle (fires on release)
 *   FUNCTION + tap track N       solo toggle
 *   hold track N + move fader N  balance, relative; volume pickup afterwards
 *   VOL +/-                      master volume, repeats while held
 *   tap PLAY                     scene layer on/off (3 s timeout)
 *     tap track N                recall scene N
 *     FUNCTION + tap track N     save scene N
 *   tap BACK / FWD               LED mode: meter / status
 *   FUNCTION + tap BACK / FWD    speaker mode: auto / off
 *   FUNCTION alone held 3 s      power off
 *   Track 1 + Track 4 held 3 s   bootloader (always, in every mode)
 */

#ifndef SP1M_UI_H_
#define SP1M_UI_H_

#include "buttons.h"
#include "fader.h"
#include "mixer.h"
#include "scene.h"

#define UI_TICK_MS		2
#define UI_LED_COUNT		8	/* 0-3 track LEDs, 4-7 playback LEDs */

#define UI_TAP_MAX_MS		600
/* Master volume ceiling at power-up, on headphone insertion and recall */
#define UI_MASTER_SAFE_DB	(-20)
#define UI_HOLD_DFU_MS		3000
#define UI_HOLD_OFF_MS		3000
#define UI_SCENE_LAYER_MS	3000
#define UI_VOL_REPEAT_DELAY_MS	400
#define UI_VOL_REPEAT_MS	80
#define UI_VOL_SHOW_MS		1500
#define UI_PERSIST_QUIET_MS	5000
#define UI_AUTO_OFF_MS		(10u * 60u * 1000u)
/* Fader travel that turns a held track key into a balance gesture */
#define UI_BAL_ENTER		16
/*
 * Ladder buttons load the supply the faders share and pull every fader
 * reading down by a few percent. Faders do not set volumes while a ladder
 * button is down, nor for UI_SAG_HOLDOFF_MS after; a held track key takes
 * its balance reference only after UI_SAG_SETTLE_MS.
 */
#define UI_SAG_HOLDOFF_MS	40
#define UI_SAG_SETTLE_MS	50
/* A raw ladder reading that is busy for longer without the debounced mask
 * ever showing a button is noise, not a press: faders work again
 */
#define UI_RAW_BUSY_MAX_MS	100
/* Balance units per fader unit while adjusting */
#define UI_BAL_PER_FADER	(200.0f / 600.0f)

enum led_mode {
	LED_MODE_METER = 0,
	LED_MODE_STATUS = 1,
};

struct ui_in {
	uint32_t now_ms;
	uint32_t buttons;		/* debounced mask, never BTN_INVALID */
	uint16_t fader[SP1M_TRACKS];	/* conditioned, 0..FADER_MAX */
	float track_peak[SP1M_TRACKS];	/* input peak since last tick, 0..1 */
	float out_peak;			/* output peak since last tick, 0..1 */
	bool ladder_busy;		/* raw ladder reading not idle (before
					 * the debounce): the supply may sag
					 */
	bool hp_present;		/* debounced jack: switches the speaker */
	bool hp_maybe;			/* latest jack reading: caps the volume */
	bool usb_configured;
	bool streaming;
	bool charging;
	bool underrun;			/* underrun since last tick */
	bool inputs_lost;		/* no ADC scan: buttons forced released */
};

struct ui_out {
	struct mix_params mix;
	uint8_t led[UI_LED_COUNT];	/* perceptual brightness 0..255 */
	bool speaker_on;
};

struct ui {
	struct persist st;

	uint32_t prev_buttons;
	uint32_t chord;			/* union of buttons since all released */
	uint32_t press_ms[BTN_COUNT];

	uint16_t hold_fader[SP1M_TRACKS];
	int8_t hold_bal[SP1M_TRACKS];
	bool bal_active[SP1M_TRACKS];
	struct pickup vol_pick[SP1M_TRACKS];

	bool scene_layer;
	uint32_t scene_layer_ms;
	int8_t last_scene;
	uint32_t confirm_until;		/* LED confirmation of a scene action */

	bool fn_alone;
	bool fn_fired;
	bool t14_active;
	uint32_t t14_ms;

	uint32_t vol_next_ms;
	uint32_t vol_show_until;
	bool hp_prev;			/* headphones maybe in on the previous tick */
	uint32_t underrun_until;

	float track_level[SP1M_TRACKS];
	float out_level;

	uint32_t last_activity_ms;
	uint16_t last_fader[SP1M_TRACKS];
	uint32_t ladder_free_ms;	/* last time a ladder button was down */
	uint32_t raw_busy_ms;		/* raw ladder reading busy since */

	bool dirty;			/* state differs from flash */
	bool dirty_urgent;		/* scene slot or setting changed */
	uint32_t dirty_ms;
	uint32_t gen;			/* bumped on every change */

	bool req_power_off;
	bool req_dfu;
};

/*
 * @loaded: state restored from flash, or NULL for defaults (then the current
 * fader positions become the track volumes).
 */
void ui_init(struct ui *u, const struct persist *loaded,
	     const uint16_t fader[SP1M_TRACKS], uint32_t now_ms);

void ui_tick(struct ui *u, const struct ui_in *in, struct ui_out *out);

/* Scene actions, also used by the serial console. Slot 0..SCENE_COUNT-1. */
bool ui_scene_save(struct ui *u, int slot, uint32_t now_ms);
bool ui_scene_recall(struct ui *u, int slot, const uint16_t fader[SP1M_TRACKS],
		     uint32_t now_ms);
void ui_set_spk_mode(struct ui *u, enum spk_mode mode, uint32_t now_ms);
/* Master volume in dB (MASTER_DB_OFF..MASTER_DB_MAX, clamped). */
void ui_set_master_db(struct ui *u, int db, uint32_t now_ms);

/*
 * External control (MIDI): the same state the buttons and faders set, so it
 * is saved, recalled and capped like theirs. A volume applies at once and
 * the fader then waits to pick it up, as after a scene recall. Track 0..3.
 */
void ui_ext_volume(struct ui *u, int track, uint16_t vol, uint16_t fader,
		   uint32_t now_ms);
void ui_ext_balance(struct ui *u, int track, int bal, uint32_t now_ms);
void ui_ext_mute(struct ui *u, int track, bool on, uint32_t now_ms);
void ui_ext_solo(struct ui *u, int track, bool on, uint32_t now_ms);
void ui_set_charge_off(struct ui *u, bool off, uint32_t now_ms);
void ui_set_led_mode(struct ui *u, enum led_mode mode, uint32_t now_ms);

/*
 * Continue after another UI instance had the controls (controller mode):
 * buttons held now count as no tap, and the faders wait to pick up the
 * stored volumes.
 */
void ui_resume(struct ui *u, const struct ui_in *in);

/* Something should be written to flash (when writing is allowed). */
bool ui_persist_due(const struct ui *u, uint32_t now_ms);
/* Copy of the state to write; @gen identifies the snapshot. */
void ui_persist_snapshot(const struct ui *u, struct persist *out, uint32_t *gen);
/* The snapshot @gen is on flash. */
void ui_persist_done(struct ui *u, uint32_t gen);

/* Requests are latched until taken. */
bool ui_take_power_off(struct ui *u);
bool ui_take_dfu(struct ui *u);

#endif /* SP1M_UI_H_ */
