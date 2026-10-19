/* Copyright (c) 2026 Ryan Gilmore. MIT License, see LICENSE. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "midi_assign.h"

static void target_tests(void)
{
    /* Exhaust every kind/channel and boundary number in both directions. */
    const uint16_t numbers[] = { 0, 1, 5, 6, 7, 31, 32, 127, 128, 16383, 16384, 65535 };
    for (int input = 0; input <= 1; ++input)
        for (int kind = 0; kind < 256; ++kind)
            for (int ch = 0; ch < 256; ++ch)
                for (unsigned n = 0; n < sizeof(numbers) / sizeof(numbers[0]); ++n) {
                    struct midi_target t = { (uint8_t)kind, (uint8_t)ch, numbers[n] };
                    int expected = 0;
                    switch (kind) {
                    case MIDI_TGT_CC: expected = t.number < 128; break;
                    case MIDI_TGT_CC14: expected = t.number < 32 && t.number != 6; break;
                    case MIDI_TGT_NRPN: case MIDI_TGT_RPN: expected = t.number < 16384; break;
                    case MIDI_TGT_PITCH_BEND: expected = t.number == 0; break;
                    case MIDI_TGT_PROGRAM: expected = t.number < 128 || (input && t.number == MIDI_NUM_ANY); break;
                    }
                    expected = expected && (ch < 16 || (input && ch == MIDI_CH_ANY));
                    assert(midi_target_valid(&t, input) == expected);
                }
    assert(!midi_target_valid(0, 1));
}

/* Independent copy of mapping.c scale(), retaining its 7-bit truncation. */
static int scale7(int position, int lo, int hi, int curve, int invert)
{
    int v = invert ? 127 - position : position;
    if (curve == MIDI_CURVE_LOG) v = v * v / 127;
    else if (curve == MIDI_CURVE_EXP) v = 127 - (127 - v) * (127 - v) / 127;
    v = lo + v * (hi - lo) / 127;
    if (v < 0) v = 0;
    if (v > 127) v = 127;
    return v;
}

static void curve_tests(void)
{
    int worst = 0;
    for (int curve = 0; curve < 3; ++curve) {
        struct midi_out_assign a = { { MIDI_TGT_CC14, 0, 20 }, 0, 16383, (uint8_t)curve, 0 };
        for (int reverse = 0; reverse < 2; ++reverse) {
            a.lo = reverse ? 15000 : 100;
            a.hi = reverse ? 100 : 15000;
            assert(midi_out_assign_map(&a, 0) == a.lo);
            assert(midi_out_assign_map(&a, 16383) == a.hi);
            uint16_t previous = a.lo;
            for (unsigned v = 0; v <= 16383; ++v) {
                uint16_t got = midi_out_assign_map(&a, (uint16_t)v);
                assert(reverse ? got <= previous : got >= previous);
                previous = got;
                a.invert = 1;
                assert(midi_out_assign_map(&a, (uint16_t)(16383 - v)) == got);
                a.invert = 0;
            }
            assert(midi_out_assign_map(&a, 16384) == a.hi);
            assert(midi_out_assign_map(&a, 65535) == a.hi);
        }
        a.lo = 0; a.hi = 16383;
        for (int invert = 0; invert < 2; ++invert) {
            a.invert = (uint8_t)invert;
            for (int x = 0; x < 128; ++x) {
                int got = midi_narrow7(midi_out_assign_map(&a, midi_widen7((uint8_t)x)));
                int delta = got - scale7(x, 0, 127, curve, invert);
                if (delta < 0) delta = -delta;
                assert(delta <= 1);
                if (delta > worst) worst = delta;
                if (curve == MIDI_CURVE_LINEAR) assert(delta == 0);
            }
        }
        a.lo = a.hi = 371;
        for (unsigned v = 0; v < 65536; ++v) assert(midi_out_assign_map(&a, (uint16_t)v) == 371);
    }
    /* Unknown curves follow mapping.c's linear fallback. */
    struct midi_out_assign a = { { MIDI_TGT_CC, 0, 0 }, 0, 16383, 255, 0 };
    assert(midi_out_assign_map(&a, 1234) == 1234);
    assert(midi_out_assign_map(0, 1) == 0);
    printf("7-bit curve oracle maximum difference: %d\n", worst);
}

static void build_tests(void)
{
    for (int kind = MIDI_TGT_CC; kind <= MIDI_TGT_PROGRAM; ++kind) {
        struct midi_out_assign a = { { (uint8_t)kind, 15, 20 }, 123, 15000, MIDI_CURVE_LOG, 1 };
        if (kind == MIDI_TGT_PITCH_BEND) a.t.number = 0;
        if (kind == MIDI_TGT_NRPN || kind == MIDI_TGT_RPN) a.t.number = 12345;
        struct midi_hr_event hr, before_hr;
        struct midi_msg msg, before_msg;
        memset(&hr, 0x5a, sizeof(hr)); before_hr = hr;
        memset(&msg, 0xa5, sizeof(msg)); before_msg = msg;
        int plain = kind == MIDI_TGT_CC || kind == MIDI_TGT_PROGRAM;
        assert(midi_out_assign_build(&a, 4321, &hr, &msg) == (plain ? 2 : 1));
        if (plain) {
            assert(memcmp(&hr, &before_hr, sizeof(hr)) == 0);
            assert(msg.status == (kind == MIDI_TGT_CC ? 0xbf : 0xcf));
            assert(msg.d1 == 20 && msg.len == (kind == MIDI_TGT_CC ? 3 : 2));
            assert(msg.d2 == (kind == MIDI_TGT_CC ? midi_narrow7(midi_out_assign_map(&a, 4321)) : 0));
            struct midi_msg other;
            assert(midi_out_assign_build(&a, 65535, 0, &other) == 2);
            if (kind == MIDI_TGT_PROGRAM) assert(memcmp(&msg, &other, sizeof(msg)) == 0);
            assert(!midi_out_assign_build(&a, 0, &hr, 0));
        } else {
            int expected = kind == MIDI_TGT_CC14 ? MIDI_HR_CC14 : kind == MIDI_TGT_NRPN ? MIDI_HR_NRPN :
                           kind == MIDI_TGT_RPN ? MIDI_HR_RPN : MIDI_HR_PITCH_BEND;
            assert(hr.kind == expected && hr.channel == 15 && hr.number == a.t.number);
            assert(hr.res == 14 && hr.value == midi_out_assign_map(&a, 4321));
            assert(memcmp(&msg, &before_msg, sizeof(msg)) == 0);
            assert(midi_out_assign_build(&a, 0, &hr, 0) == 1);
            assert(!midi_out_assign_build(&a, 0, 0, &msg));
        }
        hr = before_hr; msg = before_msg;
        a.t.channel = 16;
        assert(!midi_out_assign_build(&a, 0, &hr, &msg));
        assert(memcmp(&hr, &before_hr, sizeof(hr)) == 0 && memcmp(&msg, &before_msg, sizeof(msg)) == 0);
        a.t.channel = 0; a.lo = 65535;
        assert(!midi_out_assign_build(&a, 0, &hr, &msg));
        a.lo = 0; a.hi = 16384;
        assert(!midi_out_assign_build(&a, 0, &hr, &msg));
        a.hi = 16383; a.t.kind = MIDI_TGT_NONE;
        assert(!midi_out_assign_build(&a, 0, &hr, &msg));
    }
    assert(!midi_out_assign_build(0, 0, 0, 0));
}

int main(void)
{
    target_tests(); curve_tests(); build_tests();
    puts("all midi_assign tests passed");
    return 0;
}
