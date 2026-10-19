/* Copyright (c) 2026 Ryan Gilmore. MIT License, see LICENSE. */
#include "midi_hr.h"
#include <stdint.h>
#include <string.h>

void midi_hr_asm_init(struct midi_hr_asm *a)
{
    memset(a, 0, sizeof(*a));
}

static int emit(struct midi_hr_event *out, uint8_t kind, uint8_t ch,
                uint16_t number, uint16_t value, uint8_t res)
{
    *out = (struct midi_hr_event){ kind, ch, res, number, value };
    return 1;
}

int midi_hr_asm_feed(struct midi_hr_asm *a, const struct midi_msg *m,
                     struct midi_hr_event *out)
{
    uint8_t type = (uint8_t)(m->status & 0xf0u);
    uint8_t ch = (uint8_t)(m->status & 0x0fu);
    if (m->len != 3 || (m->d1 & 0x80u) || (m->d2 & 0x80u)) return 0;
    if (type == 0xe0u) {
        return emit(out, MIDI_HR_PITCH_BEND, ch, 0,
                    (uint16_t)(((uint16_t)m->d2 << 7) | m->d1), 14);
    }
    if (type != 0xb0u) return 0;

    uint8_t cc = m->d1;
    uint8_t v = m->d2;
    /* CC 6 / 38 are the data-entry pair: with a parameter selected they belong to the NRPN / RPN below, otherwise
     * they are an ordinary 14-bit controller (6). */
    if (cc < 64 && !((cc == 6 || cc == 38) && a->nrpn[ch].selected)) {
        uint8_t n = (uint8_t)(cc & 31u);
        a->c14[ch][n].msb = cc < 32 ? v : a->c14[ch][n].msb;
        if (cc >= 32) {
            a->c14[ch][n].lsb = v;
            a->c14[ch][n].seen_lsb = 1;
        }
        if (a->c14[ch][n].seen_lsb)
            return emit(out, MIDI_HR_CC14, ch, n,
                        (uint16_t)(((uint16_t)a->c14[ch][n].msb << 7) |
                                   a->c14[ch][n].lsb), 14);
        return emit(out, MIDI_HR_CC14, ch, n, midi_widen7(v), 7);
    }

    if (cc == 99 || cc == 98 || cc == 101 || cc == 100) {
        uint8_t rpn = (uint8_t)(cc == 101 || cc == 100);
        /* Many senders re-send only the half of the select that changed: a half of the SAME kind keeps the other half
         * (the register persists); selecting the other kind forgets both. */
        if (a->nrpn[ch].is_rpn != rpn) {
            a->nrpn[ch].have_msb = 0;
            a->nrpn[ch].have_lsb = 0;
        }
        a->nrpn[ch].is_rpn = rpn;
        a->nrpn[ch].selected = 0;
        a->nrpn[ch].data_msb = 0;
        a->nrpn[ch].data_lsb = 0;
        a->nrpn[ch].seen_data_lsb = 0;
        if (cc == 99 || cc == 101) {
            a->nrpn[ch].sel_msb = v;
            a->nrpn[ch].have_msb = 1;
        } else {
            a->nrpn[ch].sel_lsb = v;
            a->nrpn[ch].have_lsb = 1;
        }
        if (a->nrpn[ch].have_msb && a->nrpn[ch].have_lsb &&
            !(rpn && a->nrpn[ch].sel_msb == 127 && a->nrpn[ch].sel_lsb == 127))
            a->nrpn[ch].selected = 1;
        return 0;
    }
    if (!a->nrpn[ch].selected) return 0;
    uint8_t kind = a->nrpn[ch].is_rpn ? MIDI_HR_RPN : MIDI_HR_NRPN;
    uint16_t number = (uint16_t)(((uint16_t)a->nrpn[ch].sel_msb << 7) |
                                 a->nrpn[ch].sel_lsb);
    if (cc == 6) {
        a->nrpn[ch].data_msb = v;
        if (!a->nrpn[ch].seen_data_lsb)
            return emit(out, kind, ch, number, midi_widen7(v), 7);
    } else if (cc == 38) {
        a->nrpn[ch].data_lsb = v;
        a->nrpn[ch].seen_data_lsb = 1;
    } else if (cc == 96 || cc == 97) {
        uint16_t value = (uint16_t)(((uint16_t)a->nrpn[ch].data_msb << 7) |
                                     a->nrpn[ch].data_lsb);
        if (cc == 96 && value < MIDI_HR_MAX) ++value;
        if (cc == 97 && value > 0) --value;
        a->nrpn[ch].data_msb = (uint8_t)(value >> 7);
        a->nrpn[ch].data_lsb = (uint8_t)(value & 127u);
        a->nrpn[ch].seen_data_lsb = 1;
        return emit(out, kind, ch, number, value, 14);
    } else return 0;
    return emit(out, kind, ch, number,
                (uint16_t)(((uint16_t)a->nrpn[ch].data_msb << 7) |
                           a->nrpn[ch].data_lsb), 14);
}

void midi_hr_enc_init(struct midi_hr_enc *e)
{
    memset(e, 0, sizeof(*e));
}

void midi_hr_enc_forget(struct midi_hr_enc *e)
{
    midi_hr_enc_init(e);
}

static void put(struct midi_msg *out, int *n, uint8_t status,
                uint8_t d1, uint8_t d2)
{
    out[*n] = (struct midi_msg){ status, d1, d2, 3 };
    ++*n;
}

int midi_hr_expand(struct midi_hr_enc *e, const struct midi_hr_event *ev,
                   struct midi_msg *out, int cap)
{
    if (cap < MIDI_HR_EXPAND_MAX || ev->channel > 15 ||
        ev->number > MIDI_HR_MAX ||
        ev->value > MIDI_HR_MAX || (ev->res != 7 && ev->res != 14) ||
        (ev->kind != MIDI_HR_CC14 && ev->kind != MIDI_HR_NRPN &&
         ev->kind != MIDI_HR_RPN && ev->kind != MIDI_HR_PITCH_BEND) ||
        (ev->kind == MIDI_HR_CC14 && (ev->number > 31 || ev->number == 6)) ||
        (ev->kind == MIDI_HR_PITCH_BEND && ev->number != 0)) return 0;

    uint8_t status = (uint8_t)(0xb0u | ev->channel);
    uint8_t msb = ev->res == 7 ? midi_narrow7(ev->value) :
                  (uint8_t)(ev->value >> 7);
    uint8_t lsb = (uint8_t)(ev->value & 127u);
    int n = 0;
    if (ev->kind == MIDI_HR_PITCH_BEND) {
        uint16_t value = ev->res == 7 ? midi_widen7(msb) : ev->value;
        put(out, &n, (uint8_t)(0xe0u | ev->channel),
            (uint8_t)(value & 127u), (uint8_t)(value >> 7));
    } else if (ev->kind == MIDI_HR_CC14) {
        uint8_t ch = ev->channel, cc = (uint8_t)ev->number;
        if (!e->c14[ch][cc].valid_msb || e->c14[ch][cc].msb != msb)
            put(out, &n, status, cc, msb);
        if (ev->res == 14 && (!e->c14[ch][cc].valid_lsb || e->c14[ch][cc].lsb != lsb))
            put(out, &n, status, (uint8_t)(cc + 32u), lsb);
        e->c14[ch][cc].msb = msb;
        e->c14[ch][cc].valid_msb = 1;
        if (ev->res == 14) {
            e->c14[ch][cc].lsb = lsb;
            e->c14[ch][cc].valid_lsb = 1;
        }
    } else {
        uint8_t ch = ev->channel;
        uint8_t rpn = (uint8_t)(ev->kind == MIDI_HR_RPN);
        if (!e->nrpn[ch].valid || e->nrpn[ch].param != ev->number ||
            e->nrpn[ch].is_rpn != rpn) {
            put(out, &n, status, rpn ? 101 : 99, (uint8_t)(ev->number >> 7));
            put(out, &n, status, rpn ? 100 : 98, (uint8_t)(ev->number & 127u));
            e->nrpn[ch].param = ev->number;
            e->nrpn[ch].is_rpn = rpn;
            e->nrpn[ch].valid = 1;
            e->nrpn[ch].valid_msb = 0;
            e->nrpn[ch].valid_lsb = 0;
        }
        if (rpn && ev->number == MIDI_HR_MAX) return n;   /* RPN null (127 / 127) is a select only: it deselects, no data follows */
        if (!e->nrpn[ch].valid_msb || e->nrpn[ch].data_msb != msb)
            put(out, &n, status, 6, msb);
        if (ev->res == 14 && (!e->nrpn[ch].valid_lsb || e->nrpn[ch].data_lsb != lsb))
            put(out, &n, status, 38, lsb);
        e->nrpn[ch].data_msb = msb;
        e->nrpn[ch].valid_msb = 1;
        if (ev->res == 14) {
            e->nrpn[ch].data_lsb = lsb;
            e->nrpn[ch].valid_lsb = 1;
        }
    }
    return n;
}
