/* Copyright (c) 2026 Ryan Gilmore. MIT License, see LICENSE. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "midi_hr.h"

static int feed(struct midi_hr_asm *a, uint8_t status, uint8_t d1,
                uint8_t d2, struct midi_hr_event *ev)
{
    struct midi_msg m = { status, d1, d2, 3 };
    return midi_hr_asm_feed(a, &m, ev);
}

static void check(const struct midi_hr_event *ev, int kind, int ch,
                  int number, int value, int res)
{
    if (ev->kind != kind || ev->channel != ch || ev->number != number ||
        ev->value != value || ev->res != res)
        fprintf(stderr, "event got %u/%u/%u/%u/%u expected %d/%d/%d/%d/%d\n",
                ev->kind, ev->channel, ev->number, ev->value, ev->res,
                kind, ch, number, value, res);
    assert(ev->kind == kind && ev->channel == ch && ev->number == number);
    assert(ev->value == value && ev->res == res);
}

static void widen_tests(void)
{
    uint16_t prev = 0;
    for (int i = 0; i < 128; ++i) {
        uint16_t w = midi_widen7((uint8_t)i);
        assert(w >= prev && midi_narrow7(w) == i);
        prev = w;
    }
    assert(midi_widen7(0) == 0 && midi_widen7(127) == 16383);
    assert(midi_narrow7(16384) == 127 && midi_narrow7(65535) == 127);
}

static void cc_tests(void)
{
    struct midi_hr_asm a;
    struct midi_hr_event ev;
    midi_hr_asm_init(&a);
    for (int ch = 0; ch < 16; ++ch) for (int cc = 0; cc < 32; ++cc) {
        int hi = (ch * 7 + cc * 3) & 127;
        int lo = (ch * 5 + cc * 11) & 127;
        if ((cc & 1) == 0) {
            assert(feed(&a, (uint8_t)(0xb0 + ch), (uint8_t)cc, (uint8_t)hi, &ev));
            check(&ev, MIDI_HR_CC14, ch, cc, midi_widen7((uint8_t)hi), 7);
            assert(feed(&a, (uint8_t)(0xb0 + ch), (uint8_t)(cc + 32), (uint8_t)lo, &ev));
        } else {
            assert(feed(&a, (uint8_t)(0xb0 + ch), (uint8_t)(cc + 32), (uint8_t)lo, &ev));
            check(&ev, MIDI_HR_CC14, ch, cc, lo, 14);
            assert(feed(&a, (uint8_t)(0xb0 + ch), (uint8_t)cc, (uint8_t)hi, &ev));
        }
        check(&ev, MIDI_HR_CC14, ch, cc, (hi << 7) | lo, 14);
        assert(feed(&a, (uint8_t)(0xb0 + ch), (uint8_t)cc, 127, &ev));
        check(&ev, MIDI_HR_CC14, ch, cc, (127 << 7) | lo, 14);
    }
    for (int cc = 64; cc < 128; ++cc)
        assert(!feed(&a, 0xb0, (uint8_t)cc, 3, &ev));
    for (int status = 0; status < 256; ++status)
        if ((status & 0xf0) != 0xb0 && (status & 0xf0) != 0xe0)
            assert(!feed(&a, (uint8_t)status, 1, 2, &ev));
    assert(!feed(&a, 0xb0, 0x80, 1, &ev));
    assert(!feed(&a, 0xb0, 1, 0x80, &ev));
}

static void parameter_tests(void)
{
    struct midi_hr_asm a;
    struct midi_hr_event ev;
    midi_hr_asm_init(&a);
    /* with no parameter selected, CC 6 / 38 are an ordinary 14-bit controller (6) */
    assert(feed(&a, 0xb0, 6, 42, &ev));
    check(&ev, MIDI_HR_CC14, 0, 6, midi_widen7(42), 7);
    assert(feed(&a, 0xb0, 38, 7, &ev));
    check(&ev, MIDI_HR_CC14, 0, 6, (42 << 7) | 7, 14);
    assert(!feed(&a, 0xb0, 99, 1, &ev));
    assert(feed(&a, 0xb0, 6, 42, &ev));                 /* an incomplete select (one half) is no select: still the plain pair */
    check(&ev, MIDI_HR_CC14, 0, 6, (42 << 7) | 7, 14);
    assert(!feed(&a, 0xb0, 98, 2, &ev));
    assert(feed(&a, 0xb0, 6, 42, &ev));
    check(&ev, MIDI_HR_NRPN, 0, 130, midi_widen7(42), 7);
    assert(feed(&a, 0xb0, 38, 7, &ev));
    check(&ev, MIDI_HR_NRPN, 0, 130, (42 << 7) | 7, 14);
    assert(feed(&a, 0xb0, 96, 0, &ev));
    check(&ev, MIDI_HR_NRPN, 0, 130, (42 << 7) | 8, 14);
    assert(feed(&a, 0xb0, 97, 0, &ev));
    check(&ev, MIDI_HR_NRPN, 0, 130, (42 << 7) | 7, 14);
    /* a lone half of the same kind keeps the other half: re-selecting only the LSB selects 1 / 4 = 132 at once */
    assert(!feed(&a, 0xb0, 98, 4, &ev));
    assert(feed(&a, 0xb0, 38, 9, &ev));
    check(&ev, MIDI_HR_NRPN, 0, 132, 9, 14);
    assert(feed(&a, 0xb0, 6, 3, &ev));
    check(&ev, MIDI_HR_NRPN, 0, 132, (3 << 7) | 9, 14);

    assert(!feed(&a, 0xb0, 100, 3, &ev));               /* the other kind forgets both halves: not selected */
    assert(feed(&a, 0xb0, 6, 1, &ev));                  /* so CC 6 is the plain pair again */
    check(&ev, MIDI_HR_CC14, 0, 6, (1 << 7) | 7, 14);
    assert(!feed(&a, 0xb0, 101, 2, &ev));
    assert(feed(&a, 0xb0, 38, 5, &ev));
    check(&ev, MIDI_HR_RPN, 0, 259, 5, 14);
    assert(!feed(&a, 0xb0, 101, 127, &ev));
    assert(!feed(&a, 0xb0, 100, 127, &ev));
    assert(feed(&a, 0xb0, 6, 99, &ev));                 /* RPN null: deselected, plain pair again */
    check(&ev, MIDI_HR_CC14, 0, 6, (99 << 7) | 7, 14);
    assert(!feed(&a, 0xb0, 98, 1, &ev));
    assert(!feed(&a, 0xb0, 99, 0, &ev));
    assert(feed(&a, 0xb0, 97, 0, &ev));
    check(&ev, MIDI_HR_NRPN, 0, 1, 0, 14);
    assert(feed(&a, 0xb0, 6, 127, &ev));
    assert(feed(&a, 0xb0, 38, 127, &ev));
    assert(feed(&a, 0xb0, 96, 0, &ev));
    check(&ev, MIDI_HR_NRPN, 0, 1, 16383, 14);
    assert(!feed(&a, 0xb1, 99, 3, &ev));
    assert(feed(&a, 0xb1, 6, 3, &ev));                  /* one half only: not selected, the plain pair */
    check(&ev, MIDI_HR_CC14, 1, 6, midi_widen7(3), 7);
    assert(!feed(&a, 0xb1, 98, 4, &ev));
    assert(feed(&a, 0xb1, 6, 3, &ev));
    check(&ev, MIDI_HR_NRPN, 1, 388, midi_widen7(3), 7);
}

static void bend_tests(void)
{
    struct midi_hr_asm a;
    struct midi_hr_event ev;
    midi_hr_asm_init(&a);
    for (int ch = 0; ch < 16; ++ch) {
        assert(feed(&a, (uint8_t)(0xe0 + ch), 0, 64, &ev));
        check(&ev, MIDI_HR_PITCH_BEND, ch, 0, 8192, 14);
        assert(feed(&a, (uint8_t)(0xe0 + ch), 0, 0, &ev));
        check(&ev, MIDI_HR_PITCH_BEND, ch, 0, 0, 14);
        assert(feed(&a, (uint8_t)(0xe0 + ch), 127, 127, &ev));
        check(&ev, MIDI_HR_PITCH_BEND, ch, 0, 16383, 14);
    }
}

static void msg(const struct midi_msg *m, int status, int d1, int d2)
{
    assert(m->len == 3 && m->status == status && m->d1 == d1 && m->d2 == d2);
}

static void expand_tests(void)
{
    struct midi_hr_enc e;
    struct midi_msg out[MIDI_HR_EXPAND_MAX];
    struct midi_hr_event ev = { MIDI_HR_CC14, 2, 14, 5, 0x1234 };
    midi_hr_enc_init(&e);
    assert(midi_hr_expand(&e, &ev, out, MIDI_HR_EXPAND_MAX) == 2);
    msg(&out[0], 0xb2, 5, 0x24); msg(&out[1], 0xb2, 37, 0x34);
    assert(midi_hr_expand(&e, &ev, out, MIDI_HR_EXPAND_MAX) == 0);
    ev.value = 0x1334;
    assert(midi_hr_expand(&e, &ev, out, MIDI_HR_EXPAND_MAX) == 1);
    msg(&out[0], 0xb2, 5, 0x26);
    ev.value = 0x1335;
    assert(midi_hr_expand(&e, &ev, out, MIDI_HR_EXPAND_MAX) == 1);
    msg(&out[0], 0xb2, 37, 0x35);
    ev.res = 7; ev.value = midi_widen7(40);
    assert(midi_hr_expand(&e, &ev, out, MIDI_HR_EXPAND_MAX) == 1);
    msg(&out[0], 0xb2, 5, 40);
    ev.res = 14; ev.value = (40 << 7) | 0x35;
    assert(midi_hr_expand(&e, &ev, out, MIDI_HR_EXPAND_MAX) == 0);
    midi_hr_enc_forget(&e);
    assert(midi_hr_expand(&e, &ev, out, MIDI_HR_EXPAND_MAX) == 2);

    ev = (struct midi_hr_event){ MIDI_HR_NRPN, 4, 14, 0x1234, 0x2345 };
    assert(midi_hr_expand(&e, &ev, out, MIDI_HR_EXPAND_MAX) == 4);
    msg(&out[0], 0xb4, 99, 0x24); msg(&out[1], 0xb4, 98, 0x34);
    msg(&out[2], 0xb4, 6, 0x46); msg(&out[3], 0xb4, 38, 0x45);
    assert(midi_hr_expand(&e, &ev, out, MIDI_HR_EXPAND_MAX) == 0);
    ev.value = 0x2445;
    assert(midi_hr_expand(&e, &ev, out, MIDI_HR_EXPAND_MAX) == 1);
    msg(&out[0], 0xb4, 6, 0x48);
    ev.value = 0x2446;
    assert(midi_hr_expand(&e, &ev, out, MIDI_HR_EXPAND_MAX) == 1);
    msg(&out[0], 0xb4, 38, 0x46);
    ev.kind = MIDI_HR_RPN;
    assert(midi_hr_expand(&e, &ev, out, MIDI_HR_EXPAND_MAX) == 4);
    msg(&out[0], 0xb4, 101, 0x24); msg(&out[1], 0xb4, 100, 0x34);
    ev.res = 7; ev.value = midi_widen7(60);
    assert(midi_hr_expand(&e, &ev, out, MIDI_HR_EXPAND_MAX) == 1);
    msg(&out[0], 0xb4, 6, 60);
    ev.kind = MIDI_HR_PITCH_BEND; ev.number = 0; ev.res = 14; ev.value = 8192;
    assert(midi_hr_expand(&e, &ev, out, MIDI_HR_EXPAND_MAX) == 1);
    msg(&out[0], 0xe4, 0, 64);
    assert(midi_hr_expand(&e, &ev, out, MIDI_HR_EXPAND_MAX) == 1);
    struct midi_hr_enc before = e;
    ev.channel = 16; assert(!midi_hr_expand(&e, &ev, out, MIDI_HR_EXPAND_MAX));
    ev.channel = 4; ev.number = 1; assert(!midi_hr_expand(&e, &ev, out, MIDI_HR_EXPAND_MAX));
    ev.number = 0; ev.value = 16384; assert(!midi_hr_expand(&e, &ev, out, MIDI_HR_EXPAND_MAX));
    ev.value = 1; ev.kind = 255; assert(!midi_hr_expand(&e, &ev, out, MIDI_HR_EXPAND_MAX));
    ev.kind = MIDI_HR_CC14; ev.number = 32; assert(!midi_hr_expand(&e, &ev, out, MIDI_HR_EXPAND_MAX));
    ev.kind = MIDI_HR_NRPN; ev.number = 16384; assert(!midi_hr_expand(&e, &ev, out, MIDI_HR_EXPAND_MAX));
    ev.number = 1; assert(!midi_hr_expand(&e, &ev, out, MIDI_HR_EXPAND_MAX - 1));
    assert(memcmp(&before, &e, sizeof(e)) == 0);
}

static uint32_t state = 0x9e3779b9u;
static uint32_t rnd(void)
{
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

static void random_tests(void)
{
    struct midi_hr_enc e;
    struct midi_hr_asm a;
    struct midi_hr_event got;
    struct midi_msg out[MIDI_HR_EXPAND_MAX];
    midi_hr_enc_init(&e); midi_hr_asm_init(&a);
    for (int i = 0; i < 200000; ++i) {
        struct midi_hr_event ev;
        ev.kind = (uint8_t)(1 + rnd() % 4);
        ev.channel = (uint8_t)(rnd() & 15u);
        ev.number = ev.kind == MIDI_HR_CC14 ? (uint16_t)(rnd() & 31u) :
                    ev.kind == MIDI_HR_PITCH_BEND ? 0 : (uint16_t)(rnd() & 16383u);
        if (ev.kind == MIDI_HR_CC14 && ev.number == 6) ev.number = 7;
        if (ev.kind == MIDI_HR_RPN && ev.number == 16383) ev.number = 16382;
        ev.res = (rnd() & 3u) == 0 ? 7 : 14;
        ev.value = ev.res == 7 ? midi_widen7((uint8_t)(rnd() & 127u)) :
                   (uint16_t)(rnd() & 16383u);
        int n = midi_hr_expand(&e, &ev, out, MIDI_HR_EXPAND_MAX);
        int produced = 0;
        for (int j = 0; j < n; ++j)
            if (midi_hr_asm_feed(&a, &out[j], &got)) produced = 1;
        if (n && produced) {
            int expected = ev.value;
            int res = ev.res;
            if (ev.kind == MIDI_HR_PITCH_BEND) res = 14;
            else if (ev.res == 7) {
                uint8_t msb = midi_narrow7(ev.value);
                if (ev.kind == MIDI_HR_CC14 &&
                    a.c14[ev.channel][ev.number].seen_lsb) {
                    expected = (msb << 7) | a.c14[ev.channel][ev.number].lsb;
                    res = 14;
                } else if (ev.kind != MIDI_HR_CC14 &&
                           a.nrpn[ev.channel].seen_data_lsb) {
                    expected = (msb << 7) | a.nrpn[ev.channel].data_lsb;
                    res = 14;
                }
            }
            check(&got, ev.kind, ev.channel, ev.number, expected, res);
        }
    }
    /* A fresh MSB-only sender has no retained LSB. */
    midi_hr_enc_forget(&e); midi_hr_asm_init(&a);
    for (int kind = MIDI_HR_CC14; kind <= MIDI_HR_PITCH_BEND; ++kind) {
        midi_hr_enc_forget(&e); midi_hr_asm_init(&a);
        struct midi_hr_event ev = { (uint8_t)kind, 0, 7, 0,
                                    midi_widen7(103) };
        if (kind == MIDI_HR_PITCH_BEND) ev.number = 0;
        int n = midi_hr_expand(&e, &ev, out, MIDI_HR_EXPAND_MAX);
        int produced = 0;
        for (int j = 0; j < n; ++j)
            if (midi_hr_asm_feed(&a, &out[j], &got)) produced = 1;
        assert(produced);
        check(&got, kind, 0, ev.number, ev.value,
              kind == MIDI_HR_PITCH_BEND ? 14 : 7);
    }
}

static void fuzz_tests(void)
{
    struct midi_hr_asm a;
    struct midi_hr_event ev;
    midi_hr_asm_init(&a);
    for (int i = 0; i < 5000000; ++i) {
        struct midi_msg m = { (uint8_t)rnd(), (uint8_t)rnd(),
                              (uint8_t)rnd(), (uint8_t)rnd() };
        if (midi_hr_asm_feed(&a, &m, &ev)) {
            assert(ev.channel <= 15 && (ev.res == 7 || ev.res == 14));
            assert(ev.value <= 16383 && ev.kind >= MIDI_HR_CC14 &&
                   ev.kind <= MIDI_HR_PITCH_BEND);
        }
    }
}

static void reserved_tests(void)
{
    struct midi_hr_enc e;
    struct midi_msg out[MIDI_HR_EXPAND_MAX];
    midi_hr_enc_init(&e);
    /* controller 6 / 38 is the data-entry pair: a 14-bit CC on it is refused and the cache stays untouched */
    struct midi_hr_event six = { MIDI_HR_CC14, 0, 14, 6, 1234 };
    assert(midi_hr_expand(&e, &six, out, MIDI_HR_EXPAND_MAX) == 0);
    struct midi_hr_event ok = { MIDI_HR_CC14, 0, 14, 7, 1234 };
    assert(midi_hr_expand(&e, &ok, out, MIDI_HR_EXPAND_MAX) == 2);
    /* a null RPN is a valid message to send even though the assembler reports no event for it */
    struct midi_hr_event nul = { MIDI_HR_RPN, 0, 14, (127u << 7) | 127u, 0 };
    assert(midi_hr_expand(&e, &nul, out, MIDI_HR_EXPAND_MAX) == 2);   /* the select only: no data follows a null */
    assert(out[0].d1 == 101 && out[0].d2 == 127 && out[1].d1 == 100 && out[1].d2 == 127);
    struct midi_hr_asm a;
    struct midi_hr_event ev;
    midi_hr_asm_init(&a);
    assert(!midi_hr_asm_feed(&a, &out[0], &ev) && !midi_hr_asm_feed(&a, &out[1], &ev));   /* the assembler reports no event for a null */
}

int main(void)
{
    widen_tests(); cc_tests(); parameter_tests(); bend_tests();
    expand_tests(); random_tests(); fuzz_tests(); reserved_tests();
    puts("all midi_hr tests passed");
    return 0;
}
