/* Copyright (c) 2026 Ryan Gilmore. MIT License, see LICENSE. */
#ifndef MIDI_PARAMS_H
#define MIDI_PARAMS_H
#include <stdint.h>
#include "midi_msg.h"
#include "midi_hr.h"
#include "midi_bus.h"
#include "midi_assign.h"

/* midi_params.h — the parameter table an engine registers into, the input bindings that turn bus messages into writes, and the ARBITER (pickup).
 * One catch mechanism serves takeover, layer peek and external writes. Portable C99, no Zephyr, no dynamic memory, no floating point.
 *
 * A parameter is a 14-bit value (0..16383). Who wrote it matters:
 *  - PHYSICAL writes: a control's own position. These are messages with source MIDI_SRC_LOCAL (the on-device controls publish their assigned message on
 *    the bus, so the loop-back is the same path), or midi_params_write_physical. A physical write is applied only when no catch is pending.
 *  - EXTERNAL writes: every other source (USB, BLE, TRS) and midi_params_set (an engine, a scene recall). They always win, and they ARM a catch on the
 *    physical control: it is ignored until it crosses the value the external write set, then it takes over again. This is feldd's layer takeover rule
 *    (control_logic.c takeover_arm / takeover_step), generalised, with one difference: the direction is learned from the first physical sample after
 *    the arm, because the physical position is not known at the moment of the external write.
 * Not thread-safe: the caller serialises (the bus's callers are the main loop and the USB thread; an engine wraps on_msg in its own lock). */

#define MIDI_PARAMS_MAX        32
#define MIDI_PARAMS_MAX_BINDS  48
#define MIDI_PARAMS_STREAMS    4     /* hi-res assemblers, one per input stream: sources LOCAL, USB, BLE, TRS (source s uses slot s - 1) */
#define MIDI_PARAMS_DEFAULT_MARGIN 96u   /* a physical value within this many counts of the target counts as caught (14-bit counts) */

struct midi_in_bind {
    struct midi_target t;        /* what the parameter hears (channel may be MIDI_CH_ANY) */
    uint16_t param;              /* the parameter it writes, or the event id delivered to changed */
    uint8_t event;               /* 0 = state (default), 1 = event: notify on EVERY matching in-range message */
    uint16_t min, max;           /* event only: inclusive valid range in delivered value units; no scaling */
};

/* res is 7 or 14. Plain CC and PROGRAM report 7; assembled inputs preserve midi_hr_event.res; direct writes report 14.
 * CC values remain widened at res 7 (recover the original with midi_narrow7); PROGRAM values remain raw program numbers.
 * A change of resolution alone also notifies: widened CC 64 and paired 64/64 both equal 8256 but have different meanings. */
typedef void (*midi_param_changed_fn)(uint16_t param, uint16_t value14, uint8_t source, uint8_t res, void *ctx);
/* The LED hook: a physical control is (1) or no longer (0) waiting to catch up with a parameter. Called on every change of that state. */
typedef void (*midi_param_catch_fn)(uint16_t param, int catching, void *ctx);

struct midi_param_state {
    uint16_t value;
    uint8_t  res;                /* resolution of the last applied state value */
    uint8_t  catch_state;        /* 0 idle, 1 armed (waiting for the first physical sample), 2 catching (direction known) */
    uint8_t  from_above;         /* catching: the physical value started above the target */
    uint16_t target;             /* the value the catch waits for */
    uint8_t  no_pickup;          /* 1 = this parameter takes part in no catch (a switch, a toggle, a state): see midi_params_set_pickup */
};

struct midi_params {
    struct midi_param_state p[MIDI_PARAMS_MAX];
    uint16_t nparams;
    struct midi_in_bind bind[MIDI_PARAMS_MAX_BINDS];
    uint16_t nbinds;
    struct midi_hr_asm stream[MIDI_PARAMS_STREAMS];
    uint16_t margin;
    midi_param_changed_fn changed;
    midi_param_catch_fn   catch_fn;
    void *ctx;
};

/* nparams <= MIDI_PARAMS_MAX (else it is clamped). All values start at 0, resolution 14, with no catch. Callbacks may be null. */
void midi_params_init(struct midi_params *s, uint16_t nparams, midi_param_changed_fn changed, midi_param_catch_fn catch_fn, void *ctx);
void midi_params_set_margin(struct midi_params *s, uint16_t margin14);

/* Add a binding. Returns 0, or -1 when the table is full, the target is invalid as an input, the parameter is out of range, or it OVERLAPS an existing
 * binding (the same message would drive two parameters). Overlap rules: two bindings overlap when they could match the same message: same kind and
 * number on overlapping channels (MIDI_CH_ANY overlaps every channel); a CC14 binding with MSB controller n also reserves CC n and CC n+32 on its
 * channels against plain CC bindings; an NRPN / RPN binding reserves CC 6, 38, 96, 97 and 98..101 on its channels against plain CC and CC14 bindings. */
/* Event bindings require event == 1 and min <= max <= 16383 (<= 127 for PROGRAM). State bindings ignore min/max.
 * Events bypass state storage, equality suppression and pickup entirely, even if their parameter has a pending catch.
 * They leave midi_params_get and existing catch state unchanged. Example recall: { { MIDI_TGT_PROGRAM, 0, MIDI_NUM_ANY }, 0, 1, 0, 3 }.
 * Overlap rules still apply regardless of event range. Other event flag values are invalid. */
int midi_params_bind(struct midi_params *s, const struct midi_in_bind *b);

/* Pickup is for CONTINUOUS controls (a fader). A parameter driven by a button that sends an absolute state (mute, solo) must take part in no
 * catch, or an external write would leave its next button press ignored. Pickup is ENABLED by default; engines switch it off per parameter. Disabling it
 * while a catch is pending clears the catch (and calls the catch hook with 0). Returns 0, or -1 for a bad id. */
int midi_params_set_pickup(struct midi_params *s, uint16_t param, int enabled);

/* Set a parameter's value and resolution (14) at start-up WITHOUT notifying and WITHOUT arming a catch (a default). */
void midi_params_preset(struct midi_params *s, uint16_t param, uint16_t value14);

uint16_t midi_params_get(const struct midi_params *s, uint16_t param);       /* 0 for a bad id */
int midi_params_catching(const struct midi_params *s, uint16_t param);       /* 1 while a catch is armed or in progress */

/* The bus sink (midi_bus_fn compatible; ctx = the struct midi_params). Runs the message through this source's hi-res assembler, finds the binding it
 * matches (at most one, by the overlap rule) and applies it: source LOCAL = a physical write, any other source = an external write.
 *  - plain CC binding: value = midi_widen7(d2).   - CC14 / NRPN / RPN / PITCH_BEND: the assembled event's value (an MSB-alone sender is the widened 7-bit value).
 *  - PROGRAM binding: value = the program number (raw), and a MIDI_NUM_ANY binding matches every program.
 * A message from a source above MIDI_SRC_TRS has no assembler: plain CC and PROGRAM bindings still work, hi-res bindings never match it.
 * Event bindings instead notify on each match within min..max, for every source, without arbitration.
 * Anything that matches no binding is ignored (the bus still forwards it elsewhere). */
void midi_params_on_msg(const struct midi_msg *m, uint8_t source, void *ctx);

/* An engine or other non-physical writer sets a value directly (a scene recall, a default that should be caught, an arpeggiator): applied at once, `changed`
 * called if the value or resolution differs (direct writes use res 14), and a catch is armed for the physical control of this parameter.
 * Returns 1 if value or resolution changed, 0 if both were equal, -1 for a bad id. */
int midi_params_set(struct midi_params *s, uint16_t param, uint16_t value14, uint8_t source);

/* A physical control reports its position as a 14-bit value. Returns 1 if applied (and `changed` called when value or resolution differs; res 14), 0 if ignored because a catch is
 * pending (the catch completes, and the value applies, when the position reaches the target within the margin from the side it started on), -1 bad id. */
int midi_params_write_physical(struct midi_params *s, uint16_t param, uint16_t value14);
#endif
