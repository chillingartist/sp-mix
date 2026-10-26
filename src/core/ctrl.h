/*
 * SPDX-License-Identifier: MIT
 *
 * Controller mode (MIDI builds only): the faders and buttons drive a DAW
 * instead of the mixer. A second UI instance holds the DAW's side (track
 * volume, balance, mute and solo) with the same gestures and pickup as the
 * mixer, and its changes go out as the messages docs/MIDI.md lists. The
 * SP-1 then passes the host's audio at 0 dB, with only the master volume
 * applied. The mixer's own state stays as it was and comes back when the
 * mode is left. The mode is not saved: the SP-1 always starts as the mixer.
 *
 *   FUNCTION + tap PLAY   controller mode on / off
 * In controller mode, besides the mixer's gestures:
 *   tap PLAY              send the four track volumes again
 *   hold PLAY + fader N   balance of track N, absolute, once the fader has
 *                         passed the current balance (the track LED blinks
 *                         until then, and is lit once it has it)
 *   hold PLAY + tap track N   solo of track N (FUNCTION + tap still works)
 *
 * Mute and solo taps send 127 then 0, as a controller's button does, so the
 * DAW toggles on each tap; the LEDs follow the SP-1's own copy of the state,
 * or the DAW's when it sends its state back.
 *
 * Loops: a value that came in over MIDI is never sent back, and what comes
 * in for a control within CTRL_ECHO_MS of it sending is taken as its echo
 * and dropped.
 */

#ifndef SP1M_CTRL_H_
#define SP1M_CTRL_H_

#include "midi/midi_msg.h"
#include "ui.h"

/* Volume, balance, mute and solo of each track */
#define CTRL_IDS		(4 * SP1M_TRACKS)
#define CTRL_OUTBOX		32
/* A fader or balance sends at most every CTRL_RATE_MS, the latest value */
#define CTRL_RATE_MS		10
/* What arrives for a control within this long after it moved is its echo */
#define CTRL_ECHO_MS		300
/* Fader units past a 7-bit step before the sent value changes */
#define CTRL_HYST		2
/* After USB comes up, before the volumes are sent */
#define CTRL_DUMP_DELAY_MS	300
/* 7-bit steps either side of the centre that count as centre (PLAY + fader) */
#define CTRL_PAN_DETENT		2

struct ctrl {
	bool available;
	bool active;
	uint8_t channel;		/* 0..15 */
	struct ui rui;			/* the DAW's side */
	struct scene seen;		/* rui's state at the end of the last tick */

	uint8_t last[CTRL_IDS];		/* 7-bit value of each control */
	uint8_t known[CTRL_IDS];	/* what the DAW was sent or sent us */
	uint32_t due_ms[CTRL_IDS];	/* rate limit */
	uint32_t touch_ms[CTRL_IDS];	/* last physical change or send */
	bool touched[CTRL_IDS];

	bool dump_pending;
	uint32_t dump_ms;
	bool usb_prev;

	uint32_t prev_buttons;
	uint32_t chord;
	uint32_t play_ms;
	bool play_masked;		/* PLAY is ours, not rui's */

	/* The PLAY layer: balance with the faders, solo with the track keys.
	 * rui sees the faders where they were, and not those keys
	 */
	uint16_t prev_fader[SP1M_TRACKS];
	uint16_t freeze[SP1M_TRACKS];
	bool frozen;
	uint32_t thaw_ms;
	bool pan_armed;			/* the balance pickups are set */
	bool pan_pending[SP1M_TRACKS];	/* fader not yet past the balance */
	int16_t pan_side[SP1M_TRACKS];	/* fader - target when armed */
	uint16_t pan_target[SP1M_TRACKS];
	uint16_t pan_anchor[SP1M_TRACKS];
	uint8_t pan7[SP1M_TRACKS];	/* balance quantizer */
	bool pan_used;			/* a fader moved during this press */
	uint8_t trk_masked;		/* track keys pressed under PLAY */
	uint32_t trk_ms[SP1M_TRACKS];
	uint32_t trk_free_ms;

	uint8_t anim;			/* enum in ctrl.c */
	uint32_t anim_ms;

	struct midi_msg outbox[CTRL_OUTBOX];
	uint8_t ob_head;
	uint8_t ob_count;

	uint32_t sent;
	uint32_t ignored;		/* incoming messages dropped */
	uint32_t overflow;		/* outgoing messages lost to a full outbox */
};

/* Controller mode becomes available, on MIDI channel @channel (0..15). */
void ctrl_init(struct ctrl *c, uint8_t channel);

/* Replaces ui_tick(): runs the mixer UI, or the DAW's side in controller mode. */
void ctrl_tick(struct ctrl *c, struct ui *local, const struct ui_in *in,
	       struct ui_out *out);

/* The UI instance the controls drive now. */
struct ui *ctrl_ui(struct ctrl *c, struct ui *local);

/* Console: switch the mode, or send the volumes again. */
void ctrl_set_mode(struct ctrl *c, struct ui *local, const struct ui_in *in,
		   bool on);
void ctrl_request_dump(struct ctrl *c, uint32_t now_ms);

/*
 * An incoming message, before it is applied to ctrl_ui(): false drops it
 * (in controller mode: an echo of a control that just moved, or a program
 * change, as there are no scenes for the DAW).
 */
bool ctrl_filter_in(struct ctrl *c, const struct midi_msg *m, uint32_t now_ms);

/* Next message to send to the host. */
bool ctrl_take(struct ctrl *c, struct midi_msg *m);

/* 7-bit values as sent, and their inverses (exact for every 7-bit value) */
uint8_t ctrl_vol7(uint16_t fader);
uint8_t ctrl_bal7(int bal);

#endif /* SP1M_CTRL_H_ */
