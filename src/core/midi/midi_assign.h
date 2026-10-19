/* Copyright (c) 2026 Ryan Gilmore. MIT License, see LICENSE. */
#ifndef MIDI_ASSIGN_H
#define MIDI_ASSIGN_H
#include <stdint.h>
#include "midi_msg.h"
#include "midi_hr.h"

/* midi_assign.h — the assignment record, read in two directions. A CONTROL has an output assignment: the message it sends. A PARAMETER has an
 * input binding (midi_params.h): the message it hears. Both name a TARGET. The profile format will serialise this (IDEAS.md #30, Phase 1).
 * Portable C99. */

enum midi_target_kind {
    MIDI_TGT_NONE = 0,
    MIDI_TGT_CC,            /* a plain 7-bit controller: number 0..127 */
    MIDI_TGT_CC14,          /* a 14-bit controller: number = the MSB controller 0..31, not 6 (6 / 38 are the data-entry pair); the LSB is number + 32 */
    MIDI_TGT_NRPN,          /* number = the parameter 0..16383 */
    MIDI_TGT_RPN,           /* as NRPN */
    MIDI_TGT_PITCH_BEND,    /* number 0 */
    MIDI_TGT_PROGRAM        /* number = the program 0..127; as an INPUT, MIDI_NUM_ANY matches every program and delivers the program number as the value (raw, 0..127) */
};

#define MIDI_CH_ANY   0xFFu     /* an input binding may listen on any channel (never valid for an output) */
#define MIDI_NUM_ANY  0xFFFFu

struct midi_target {
    uint8_t  kind;              /* enum midi_target_kind */
    uint8_t  channel;           /* 0..15, or MIDI_CH_ANY (input only) */
    uint16_t number;
};

enum midi_curve { MIDI_CURVE_LINEAR = 0, MIDI_CURVE_LOG = 1, MIDI_CURVE_EXP = 2 };

/* What a control sends: its position (0..16383) is shaped by `curve` and `invert`, then mapped onto lo..hi (lo may exceed hi: that reverses it). */
struct midi_out_assign {
    struct midi_target t;
    uint16_t lo, hi;            /* 0..16383 */
    uint8_t  curve;             /* enum midi_curve */
    uint8_t  invert;            /* 1 = flip the position first */
};

/* 1 if the target is well formed (for_input: MIDI_CH_ANY and MIDI_NUM_ANY are allowed where documented), else 0. */
int midi_target_valid(const struct midi_target *t, int for_input);

/* Shape and map a control position (0..16383) through an assignment's curve, invert and lo..hi: the 14-bit value the control sends.
 * The curves must match feldd's existing fader curves (mapping.c, `scale`) at 7-bit resolution: read that function first and reproduce its shapes
 * in integer arithmetic at 14 bits (monotonic, endpoints exact, invert symmetric). A position above 16383 is clamped. */
uint16_t midi_out_assign_map(const struct midi_out_assign *a, uint16_t control14);

/* Turn a control position into what to send: 1 = *hr is filled (CC14, NRPN, RPN, PITCH_BEND: expand it with midi_hr_expand); 2 = *msg is filled
 * (a plain CC, or a PROGRAM change: number = the program, d1 only); 0 = invalid assignment (nothing filled). For MIDI_TGT_PROGRAM the program sent is the
 * target's number and the control value is ignored. A plain CC sends midi_narrow7 of the mapped value. */
int midi_out_assign_build(const struct midi_out_assign *a, uint16_t control14, struct midi_hr_event *hr, struct midi_msg *msg);
#endif
