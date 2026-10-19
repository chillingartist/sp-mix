/* Copyright (c) 2026 Ryan Gilmore. MIT License, see LICENSE. */
#include "midi_assign.h"

int midi_target_valid(const struct midi_target *t, int for_input)
{
    if (!t || (t->channel > 15 && !(for_input && t->channel == MIDI_CH_ANY))) return 0;
    switch (t->kind) {
    case MIDI_TGT_CC: return t->number <= 127;
    case MIDI_TGT_CC14: return t->number <= 31 && t->number != 6;
    case MIDI_TGT_NRPN: case MIDI_TGT_RPN: return t->number <= MIDI_HR_MAX;
    case MIDI_TGT_PITCH_BEND: return t->number == 0;
    case MIDI_TGT_PROGRAM:
        return t->number <= 127 || (for_input && t->number == MIDI_NUM_ANY);
    default: return 0;
    }
}

static uint16_t clamp14(uint16_t v)
{
    return v > MIDI_HR_MAX ? (uint16_t)MIDI_HR_MAX : v;
}

uint16_t midi_out_assign_map(const struct midi_out_assign *a, uint16_t control14)
{
    if (!a) return 0;
    uint32_t v = clamp14(control14);
    if (a->invert) v = MIDI_HR_MAX - v;
    /* Same quadratic shapes and truncation as mapping.c, with 16383 replacing 127. */
    if (a->curve == MIDI_CURVE_LOG) v = v * v / MIDI_HR_MAX;
    else if (a->curve == MIDI_CURVE_EXP) {
        uint32_t complement = MIDI_HR_MAX - v;
        v = MIDI_HR_MAX - complement * complement / MIDI_HR_MAX;
    }
    int32_t lo = clamp14(a->lo), hi = clamp14(a->hi);
    return (uint16_t)(lo + (int32_t)v * (hi - lo) / (int32_t)MIDI_HR_MAX);
}

int midi_out_assign_build(const struct midi_out_assign *a, uint16_t control14,
                          struct midi_hr_event *hr, struct midi_msg *msg)
{
    if (!a || !midi_target_valid(&a->t, 0) || a->lo > MIDI_HR_MAX ||
        a->hi > MIDI_HR_MAX) return 0;
    if (a->t.kind == MIDI_TGT_CC || a->t.kind == MIDI_TGT_PROGRAM) {
        if (!msg) return 0;
        *msg = (struct midi_msg){ (uint8_t)((a->t.kind == MIDI_TGT_CC ? 0xb0u : 0xc0u) | a->t.channel),
                                  (uint8_t)a->t.number, 0,
                                  (uint8_t)(a->t.kind == MIDI_TGT_CC ? 3 : 2) };
        if (a->t.kind == MIDI_TGT_CC) msg->d2 = midi_narrow7(midi_out_assign_map(a, control14));
        return 2;
    }
    if (!hr) return 0;
    uint8_t kind;
    switch (a->t.kind) {
    case MIDI_TGT_CC14: kind = MIDI_HR_CC14; break;
    case MIDI_TGT_NRPN: kind = MIDI_HR_NRPN; break;
    case MIDI_TGT_RPN: kind = MIDI_HR_RPN; break;
    default: kind = MIDI_HR_PITCH_BEND; break;
    }
    *hr = (struct midi_hr_event){ kind, a->t.channel, 14, a->t.number,
                                 midi_out_assign_map(a, control14) };
    return 1;
}
