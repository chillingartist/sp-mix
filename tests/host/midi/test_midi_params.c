/* Copyright (c) 2026 Ryan Gilmore. MIT License, see LICENSE. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "midi_params.h"

struct log {
    unsigned changes, catches;
    uint16_t param, value;
    uint8_t source, res;
    int catching;
    uint16_t limit;
};
static void changed(uint16_t param, uint16_t value, uint8_t source, uint8_t res, void *ctx)
{
    struct log *l = ctx;
    assert(param < l->limit && value <= 16383);
    assert(res == 7 || res == 14);
    l->res = res;
    ++l->changes; l->param = param; l->value = value; l->source = source;
}
static void caught(uint16_t param, int catching, void *ctx)
{
    struct log *l = ctx;
    assert(param < l->limit && (catching == 0 || catching == 1));
    ++l->catches; l->param = param; l->catching = catching;
}
static void init(struct midi_params *s, struct log *l, uint16_t n)
{
    memset(l, 0, sizeof(*l)); l->limit = n > MIDI_PARAMS_MAX ? MIDI_PARAMS_MAX : n;
    midi_params_init(s, n, changed, caught, l);
}
static int bind(struct midi_params *s, uint8_t kind, uint8_t ch, uint16_t num, uint16_t param)
{
    struct midi_in_bind b = { { kind, ch, num }, param, 0, 0, 0 };
    return midi_params_bind(s, &b);
}
static void send(struct midi_params *s, uint8_t source, uint8_t status, uint8_t d1, uint8_t d2)
{
    struct midi_msg m = { status, d1, d2, (uint8_t)((status & 0xf0u) == 0xc0u ? 2 : 3) };
    midi_params_on_msg(&m, source, s);
}
static void mixer(struct midi_params *s, struct log *l)
{
    init(s, l, 18);
    for (unsigned i = 0; i < 9; ++i) assert(bind(s, MIDI_TGT_CC14, 0, (uint16_t)(20 + i), (uint16_t)i) == 0);
    for (unsigned i = 0; i < 8; ++i) assert(bind(s, MIDI_TGT_CC, 0, (uint16_t)(102 + i), (uint16_t)(9 + i)) == 0);
    assert(bind(s, MIDI_TGT_PROGRAM, 0, MIDI_NUM_ANY, 17) == 0);
}

static void mixer_tests(void)
{
    struct midi_params s;
    struct log l;
    mixer(&s, &l);
    for (int i = 0; i < 9; ++i) {
        send(&s, MIDI_SRC_USB, 0xb0, (uint8_t)(20 + i), (uint8_t)(40 + i));
        assert(midi_params_get(&s, (uint16_t)i) == midi_widen7((uint8_t)(40 + i)));
        send(&s, MIDI_SRC_USB, 0xb0, (uint8_t)(52 + i), (uint8_t)(15 + i));
        assert(midi_params_get(&s, (uint16_t)i) == ((40 + i) << 7) + 15 + i);
        assert(l.param == i && l.source == MIDI_SRC_USB);
    }
    for (int i = 0; i < 8; ++i) {
        send(&s, MIDI_SRC_USB, 0xb0, (uint8_t)(102 + i), (uint8_t)(120 - i));
        assert(midi_params_get(&s, (uint16_t)(9 + i)) == midi_widen7((uint8_t)(120 - i)));
    }
    for (int i = 0; i < 128; ++i) {
        send(&s, MIDI_SRC_USB, 0xc0, (uint8_t)i, 255); /* unused d2 is ignored */
        assert(midi_params_get(&s, 17) == i);
    }
    unsigned changes = l.changes, catches = l.catches;
    send(&s, MIDI_SRC_USB, 0xc0, 127, 0);
    assert(l.changes == changes && l.catches == catches);
    uint16_t values[18];
    for (unsigned i = 0; i < 18; ++i) values[i] = midi_params_get(&s, (uint16_t)i);
    send(&s, MIDI_SRC_USB, 0xb1, 20, 0);
    send(&s, MIDI_SRC_USB, 0xc1, 0, 0);
    send(&s, MIDI_SRC_USB, 0xb0, 110, 127);
    for (unsigned i = 0; i < 18; ++i) assert(midi_params_get(&s, (uint16_t)i) == values[i]);
    assert(l.changes == changes);
}

static void overlap_pair(struct midi_target a, struct midi_target b, int overlap)
{
    for (int reverse = 0; reverse < 2; ++reverse) {
        struct midi_params s;
        midi_params_init(&s, 2, 0, 0, 0);
        struct midi_in_bind x = { reverse ? b : a, 0, 0, 0, 0 }, y = { reverse ? a : b, 1, 0, 0, 0 };
        assert(midi_params_bind(&s, &x) == 0);
        assert(midi_params_bind(&s, &y) == (overlap ? -1 : 0));
        /* Duplicate binding is refused even when it points to the same parameter. */
        assert(midi_params_bind(&s, &x) == -1);
    }
}
static void binding_tests(void)
{
    for (int kind = MIDI_TGT_CC; kind <= MIDI_TGT_PROGRAM; ++kind) {
        uint16_t n = kind == MIDI_TGT_PITCH_BEND ? 0 : 20;
        struct midi_target a = { (uint8_t)kind, 0, n }, b = a;
        overlap_pair(a, b, 1);
        b.channel = 1; overlap_pair(a, b, 0);
        b.channel = MIDI_CH_ANY; overlap_pair(a, b, 1);
        a.channel = MIDI_CH_ANY; overlap_pair(a, b, 1);
        if (kind != MIDI_TGT_PITCH_BEND) {
            b.number++; overlap_pair(a, b, 0);
        }
    }
    overlap_pair((struct midi_target){ MIDI_TGT_PROGRAM, 0, MIDI_NUM_ANY },
                 (struct midi_target){ MIDI_TGT_PROGRAM, MIDI_CH_ANY, 37 }, 1);
    overlap_pair((struct midi_target){ MIDI_TGT_PROGRAM, 0, MIDI_NUM_ANY },
                 (struct midi_target){ MIDI_TGT_PROGRAM, 1, 37 }, 0);
    /* Exhaust all legal CC14 / plain CC intersections, on shared and separate channels. */
    for (unsigned n = 0; n < 32; ++n) if (n != 6)
        for (unsigned cc = 0; cc < 128; ++cc) {
            struct midi_target a = { MIDI_TGT_CC14, MIDI_CH_ANY, (uint16_t)n };
            struct midi_target b = { MIDI_TGT_CC, 3, (uint16_t)cc };
            overlap_pair(a, b, cc == n || cc == n + 32);
            a.channel = 4; overlap_pair(a, b, 0);
        }
    for (int kind = MIDI_TGT_NRPN; kind <= MIDI_TGT_RPN; ++kind) {
        for (unsigned cc = 0; cc < 128; ++cc) {
            int reserved = cc == 6 || cc == 38 || (cc >= 96 && cc <= 101);
            struct midi_target a = { (uint8_t)kind, 0, 130 }, b = { MIDI_TGT_CC, 0, (uint16_t)cc };
            overlap_pair(a, b, reserved);
            b.channel = MIDI_CH_ANY; overlap_pair(a, b, reserved);
            b.channel = 1; overlap_pair(a, b, 0);
        }
        for (unsigned n = 0; n < 32; ++n) if (n != 6)
            overlap_pair((struct midi_target){ (uint8_t)kind, 0, 130 },
                         (struct midi_target){ MIDI_TGT_CC14, 0, (uint16_t)n }, 0);
    }
    overlap_pair((struct midi_target){ MIDI_TGT_NRPN, 0, 130 }, (struct midi_target){ MIDI_TGT_RPN, 0, 130 }, 0);
    struct midi_params s;
    midi_params_init(&s, 2, 0, 0, 0);
    assert(bind(&s, MIDI_TGT_CC, 0, 1, 2) == -1);
    assert(bind(&s, MIDI_TGT_CC, 0, 1, 65535) == -1);
    assert(bind(&s, MIDI_TGT_CC14, 0, 6, 0) == -1);
    assert(bind(&s, MIDI_TGT_CC, 16, 1, 0) == -1);
    assert(bind(&s, MIDI_TGT_NONE, 0, 1, 0) == -1);
    assert(midi_params_bind(&s, 0) == -1 && midi_params_bind(0, 0) == -1);
    for (unsigned i = 0; i < MIDI_PARAMS_MAX_BINDS; ++i)
        assert(bind(&s, MIDI_TGT_CC, (uint8_t)(i / 16), (uint16_t)(64 + i % 16), (uint16_t)(i % 2)) == 0);
    assert(bind(&s, MIDI_TGT_CC, 15, 127, 0) == -1);
    assert(s.nbinds == MIDI_PARAMS_MAX_BINDS);
}

static void arbiter_tests(void)
{
    struct midi_params s;
    struct log l;
    init(&s, &l, 2);
    assert(s.margin == 96 && !midi_params_catching(&s, 0) && midi_params_get(&s, 0) == 0);
    midi_params_preset(&s, 0, 5000);
    assert(l.changes == 0 && l.catches == 0 && !midi_params_catching(&s, 0));
    assert(midi_params_set(&s, 0, 8000, MIDI_SRC_USB) == 1);
    assert(l.changes == 1 && l.value == 8000 && l.source == MIDI_SRC_USB);
    assert(l.catches == 1 && l.catching && midi_params_catching(&s, 0));
    assert(midi_params_write_physical(&s, 0, 12000) == 0);
    assert(l.catches == 1 && midi_params_get(&s, 0) == 8000);
    assert(midi_params_write_physical(&s, 0, 9000) == 0);
    assert(midi_params_write_physical(&s, 0, 8000 + 96) == 1);
    assert(l.changes == 2 && l.source == MIDI_SRC_LOCAL && l.catches == 2 && !l.catching);
    assert(!midi_params_catching(&s, 0));
    assert(midi_params_write_physical(&s, 0, 100) == 1 && midi_params_get(&s, 0) == 100);
    unsigned changes = l.changes;
    assert(midi_params_write_physical(&s, 0, 100) == 1 && l.changes == changes);
    /* Re-arm after an equal external write, then catch immediately inside the margin. */
    assert(midi_params_set(&s, 0, 100, MIDI_SRC_ENGINE) == 0);
    assert(l.changes == changes && l.catches == 3);
    assert(midi_params_write_physical(&s, 0, 4) == 1 && l.catches == 4);
    assert(midi_params_set(&s, 0, 8000, MIDI_SRC_USB) == 1);
    assert(midi_params_write_physical(&s, 0, 1000) == 0);
    assert(midi_params_write_physical(&s, 0, 7903) == 0);
    assert(midi_params_write_physical(&s, 0, 7904) == 1);
    /* Jumps across the target from either direction catch too. */
    for (int above = 0; above < 2; ++above) {
        midi_params_set(&s, 0, 8000, MIDI_SRC_BLE);
        assert(midi_params_write_physical(&s, 0, above ? 12000 : 1000) == 0);
        assert(midi_params_write_physical(&s, 0, above ? 1000 : 12000) == 1);
    }
    midi_params_set(&s, 0, 8000, MIDI_SRC_USB);
    assert(midi_params_write_physical(&s, 0, 12000) == 0);
    unsigned catches = l.catches;
    midi_params_set(&s, 0, 15000, MIDI_SRC_ENGINE);
    assert(l.catches == catches && s.p[0].catch_state == 1);
    assert(midi_params_write_physical(&s, 0, 10000) == 0); /* direction learned afresh */
    assert(midi_params_write_physical(&s, 0, 15100) == 1);
    midi_params_set_margin(&s, 0);
    midi_params_set(&s, 0, 8000, MIDI_SRC_USB);
    assert(midi_params_write_physical(&s, 0, 8001) == 0);
    assert(midi_params_write_physical(&s, 0, 8000) == 1);
    midi_params_set_margin(&s, 65535); assert(s.margin == 16383);
    assert(midi_params_set(&s, 0, 65535, MIDI_SRC_TRS) == 1 && midi_params_get(&s, 0) == 16383);
    assert(midi_params_write_physical(&s, 0, 65535) == 1 && midi_params_get(&s, 0) == 16383);
    /* LOCAL bus writes and the direct physical entry point have identical effects and hooks. */
    struct midi_params t;
    struct log k;
    init(&s, &l, 1); init(&t, &k, 1);
    assert(bind(&s, MIDI_TGT_CC, 0, 102, 0) == 0);
    midi_params_set(&s, 0, 8000, MIDI_SRC_USB); midi_params_set(&t, 0, 8000, MIDI_SRC_USB);
    const uint8_t samples[] = { 10, 40, 62, 70, 70, 30 };
    for (unsigned i = 0; i < sizeof(samples); ++i) {
        send(&s, MIDI_SRC_LOCAL, 0xb0, 102, samples[i]);
        midi_params_write_physical(&t, 0, midi_widen7(samples[i]));
        /* Bus CC has res 7; the direct physical API takes res 14. Compare value and pickup behaviour. */
        struct midi_param_state bus_state = s.p[0], direct_state = t.p[0];
        bus_state.res = direct_state.res;
        assert(memcmp(&bus_state, &direct_state, sizeof(bus_state)) == 0);
        assert(l.changes == k.changes && l.catches == k.catches && l.source == k.source);
    }
}

static void stream_tests(void)
{
    struct midi_params s;
    struct log l;
    init(&s, &l, 4);
    assert(bind(&s, MIDI_TGT_CC14, MIDI_CH_ANY, 20, 0) == 0);
    assert(bind(&s, MIDI_TGT_CC, MIDI_CH_ANY, 102, 1) == 0);
    assert(bind(&s, MIDI_TGT_PROGRAM, MIDI_CH_ANY, 9, 2) == 0);
    for (uint8_t source = MIDI_SRC_ENGINE; source <= 7; ++source) {
        send(&s, source, 0xb0, 20, 127); send(&s, source, 0xb0, 52, 127);
        assert(midi_params_get(&s, 0) == 0);
        send(&s, source, 0xb0, 102, source);
        assert(midi_params_get(&s, 1) == midi_widen7(source) && l.source == source);
        send(&s, source, 0xc0, 8, 0); assert(midi_params_get(&s, 2) == (source == 5 ? 0 : 9));
        send(&s, source, 0xc0, 9, 0); assert(midi_params_get(&s, 2) == 9);
    }
    send(&s, MIDI_SRC_USB, 0xb3, 20, 90);
    assert(midi_params_get(&s, 0) == midi_widen7(90));
    midi_params_set_margin(&s, 16383); /* allow LOCAL's independent LSB event to take over */
    send(&s, MIDI_SRC_LOCAL, 0xb3, 52, 7);
    assert(midi_params_get(&s, 0) == 7);
    send(&s, MIDI_SRC_USB, 0xb3, 52, 11);
    assert(midi_params_get(&s, 0) == ((90 << 7) | 11));
    send(&s, MIDI_SRC_LOCAL, 0xb3, 20, 30);
    assert(midi_params_get(&s, 0) == ((30 << 7) | 7));
    send(&s, MIDI_SRC_BLE, 0xb3, 20, 40);
    assert(midi_params_get(&s, 0) == midi_widen7(40));
    send(&s, MIDI_SRC_TRS, 0xb3, 20, 50);
    assert(midi_params_get(&s, 0) == midi_widen7(50));
    send(&s, MIDI_SRC_USB, 0xb4, 52, 3);
    assert(midi_params_get(&s, 0) == 3); /* channels independent as well */
    /* All other assembled event types reach their bindings. */
    init(&s, &l, 4);
    assert(bind(&s, MIDI_TGT_NRPN, 0, 130, 0) == 0);
    assert(bind(&s, MIDI_TGT_RPN, 0, 259, 1) == 0);
    assert(bind(&s, MIDI_TGT_PITCH_BEND, MIDI_CH_ANY, 0, 2) == 0);
    send(&s, MIDI_SRC_USB, 0xb0, 99, 1); send(&s, MIDI_SRC_USB, 0xb0, 98, 2);
    send(&s, MIDI_SRC_USB, 0xb0, 6, 55);
    assert(midi_params_get(&s, 0) == midi_widen7(55) && l.res == 7);
    send(&s, MIDI_SRC_USB, 0xb0, 38, 10);
    assert(midi_params_get(&s, 0) == ((55 << 7) | 10) && l.res == 14);
    send(&s, MIDI_SRC_USB, 0xb0, 96, 0);
    assert(midi_params_get(&s, 0) == ((55 << 7) | 11));
    send(&s, MIDI_SRC_BLE, 0xb0, 100, 3); send(&s, MIDI_SRC_BLE, 0xb0, 101, 2);
    send(&s, MIDI_SRC_BLE, 0xb0, 38, 9); send(&s, MIDI_SRC_BLE, 0xb0, 6, 44);
    assert(midi_params_get(&s, 1) == ((44 << 7) | 9));
    send(&s, MIDI_SRC_USB, 0xb0, 6, 56);
    assert(midi_params_get(&s, 0) == ((56 << 7) | 11));
    send(&s, MIDI_SRC_TRS, 0xef, 1, 64);
    assert(midi_params_get(&s, 2) == 8193 && l.source == MIDI_SRC_TRS && l.res == 14);
    for (uint8_t source = 5; source <= 7; ++source) {
        unsigned n = l.changes;
        send(&s, source, 0xef, 127, 127);
        send(&s, source, 0xb0, 99, 1); send(&s, source, 0xb0, 98, 2); send(&s, source, 0xb0, 6, 0);
        assert(l.changes == n);
    }
}

struct output { unsigned calls; struct midi_msg last; uint8_t source; };
static void trs(const struct midi_msg *m, void *ctx)
{
    struct output *o = ctx; ++o->calls; o->last = *m;
}
static void usb(const struct midi_msg *m, uint8_t source, void *ctx)
{
    assert(source != MIDI_SRC_USB);
    struct output *o = ctx; trs(m, ctx); o->source = source;
}
static void end_to_end(void)
{
    struct midi_params s;
    struct log l;
    struct midi_bus bus;
    struct output tr = {0}, us = {0};
    mixer(&s, &l); midi_bus_init(&bus);
    assert(midi_bus_add(&bus, midi_params_on_msg, &s, MIDI_SRC_ALL, 0) == 0);
    assert(midi_bus_add_legacy(&bus, trs, &tr, MIDI_SRC_ALL, MIDI_SRC_TRS) == 1);
    assert(midi_bus_add(&bus, usb, &us, MIDI_SRC_ALL, MIDI_SRC_USB) == 2);
    struct midi_out_assign a = { { MIDI_TGT_CC14, 0, 20 }, 0, 16383, MIDI_CURVE_LINEAR, 0 };
    struct midi_hr_enc encoder;
    struct midi_hr_event ev;
    struct midi_msg out[MIDI_HR_EXPAND_MAX];
    midi_hr_enc_init(&encoder);
    assert(midi_out_assign_build(&a, 12000, &ev, 0) == 1);
    int n = midi_hr_expand(&encoder, &ev, out, MIDI_HR_EXPAND_MAX);
    assert(n == 2);
    for (int i = 0; i < n; ++i) midi_bus_publish(&bus, MIDI_SRC_LOCAL, &out[i]);
    assert(midi_params_get(&s, 0) == 12000 && tr.calls == 2 && us.calls == 2 && us.source == MIDI_SRC_LOCAL);
    struct midi_msg daw[] = { {0xb0, 20, 62, 3}, {0xb0, 52, 64, 3} }; /* 8000 */
    midi_bus_publish(&bus, MIDI_SRC_USB, &daw[0]); midi_bus_publish(&bus, MIDI_SRC_USB, &daw[1]);
    assert(midi_params_get(&s, 0) == 8000 && midi_params_catching(&s, 0));
    assert(tr.calls == 4 && us.calls == 2 && bus.suppressed == 2);
    /* A physical MSB above the target is suppressed by pickup, but still routed to outputs. */
    struct midi_msg physical = { 0xb0, 20, 90, 3 };
    midi_bus_publish(&bus, MIDI_SRC_LOCAL, &physical);
    assert(midi_params_get(&s, 0) == 8000 && midi_params_catching(&s, 0));
    physical.d2 = 60; midi_bus_publish(&bus, MIDI_SRC_LOCAL, &physical);
    assert(midi_params_get(&s, 0) == ((60 << 7) | (12000 & 127)) && !midi_params_catching(&s, 0));
    assert(tr.calls == 6 && us.calls == 4 && l.source == MIDI_SRC_LOCAL);
    midi_bus_publish(&bus, MIDI_SRC_TRS, &physical);
    assert(tr.calls == 6 && us.calls == 5 && us.source == MIDI_SRC_TRS && bus.suppressed == 3);
    assert(bus.published == 7 && bus.delivered == 18 && bus.rejected == 0);
}

static void invalid_tests(void)
{
    struct midi_params s;
    struct log l;
    init(&s, &l, 65535); assert(s.nparams == 32);
    assert(bind(&s, MIDI_TGT_CC, 0, 20, 0) == 0);
    assert(bind(&s, MIDI_TGT_PROGRAM, 0, MIDI_NUM_ANY, 1) == 0);
    for (unsigned id = 32; id < 65536; ++id) {
        assert(midi_params_set(&s, (uint16_t)id, 1, 2) == -1);
        assert(midi_params_write_physical(&s, (uint16_t)id, 1) == -1);
        midi_params_preset(&s, (uint16_t)id, 1);
        assert(!midi_params_get(&s, (uint16_t)id) && !midi_params_catching(&s, (uint16_t)id));
    }
    for (unsigned len = 0; len < 256; ++len) {
        struct midi_msg m = { 0xb0, 20, 127, (uint8_t)len };
        if (len != 3) midi_params_on_msg(&m, MIDI_SRC_USB, &s);
        m.status = 0xc0;
        if (len != 2) midi_params_on_msg(&m, MIDI_SRC_USB, &s);
    }
    for (unsigned x = 128; x < 256; ++x) {
        send(&s, 2, 0xb0, 20, (uint8_t)x);
        send(&s, 2, 0xb0, (uint8_t)x, 127);
        send(&s, 2, 0xc0, (uint8_t)x, 0);
    }
    for (unsigned source = 0; source < 256; ++source)
        if (!source || source > 7) send(&s, (uint8_t)source, 0xb0, 20, 127);
    assert(l.changes == 0 && l.catches == 0);
    midi_params_on_msg(0, 2, &s); midi_params_on_msg(0, 2, 0);
    midi_params_init(0, 1, 0, 0, 0); midi_params_set_margin(0, 0);
    assert(midi_params_set(0, 0, 0, 0) == -1 && midi_params_write_physical(0, 0, 0) == -1);
    assert(!midi_params_get(0, 0) && !midi_params_catching(0, 0));
    midi_params_preset(0, 0, 0);
    midi_params_init(&s, 0, 0, 0, 0);
    assert(bind(&s, MIDI_TGT_CC, 0, 20, 0) == -1);
    midi_params_init(&s, 1, 0, 0, 0);
    midi_params_preset(&s, 0, 65535); assert(midi_params_get(&s, 0) == 16383);
    assert(midi_params_set(&s, 0, 5, 2) == 1);
    assert(midi_params_write_physical(&s, 0, 5) == 1);
}

static uint32_t rnd(uint32_t *state)
{
    *state ^= *state << 13; *state ^= *state >> 17; *state ^= *state << 5;
    return *state;
}
static void fuzz_tests(void)
{
    struct midi_params s;
    struct log l;
    struct midi_bus bus;
    mixer(&s, &l); midi_bus_init(&bus);
    assert(midi_bus_add(&bus, midi_params_on_msg, &s, MIDI_SRC_ALL, 0) == 0);
    uint32_t state = 0x9e3779b9u;
    for (unsigned i = 0; i < 5000000; ++i) {
        struct midi_msg m = { (uint8_t)rnd(&state), (uint8_t)rnd(&state),
                              (uint8_t)rnd(&state), (uint8_t)rnd(&state) };
        uint8_t source = (uint8_t)rnd(&state);
        /* Mix unconstrained garbage with valid and nearly-valid messages so routing and writes get exercised heavily. */
        if ((i & 3u) == 0) {
            source &= 7;
            m.status = (uint8_t)(0xb0u | (rnd(&state) & 1u));
            m.len = 3; m.d1 &= 127; m.d2 &= 127;
        }
        if ((i & 31u) == 1) {
            source &= 7; m.status = 0xc0; m.len = 2; m.d1 &= 127;
        }
        midi_bus_publish(&bus, source, (i & 1023u) == 0 ? 0 : &m);
        for (unsigned j = 0; j < MIDI_PARAMS_MAX; ++j) {
            assert(s.p[j].value <= 16383);
            if (j >= s.nparams) assert(s.p[j].value == 0 && s.p[j].catch_state == 0);
        }
    }
    assert(bus.published + bus.rejected == 5000000 && bus.delivered == bus.published);
    assert(l.changes > 1000);
}

static int g_catch_calls;
static void count_catch(uint16_t param, int catching, void *ctx) { (void)param; (void)catching; (void)ctx; ++g_catch_calls; }

static void no_pickup_tests(void)
{
    struct midi_params s;
    midi_params_init(&s, 4, 0, count_catch, 0);
    g_catch_calls = 0;
    struct midi_in_bind b = { { MIDI_TGT_CC, 0, 102 }, 0, 0, 0, 0 };   /* a mute switch */
    assert(midi_params_bind(&s, &b) == 0);
    assert(midi_params_set_pickup(&s, 0, 0) == 0);
    assert(midi_params_set_pickup(&s, 99, 0) == -1);
    assert(midi_params_set(&s, 0, 16383, MIDI_SRC_USB) == 1);   /* an external write: no catch is armed */
    assert(!midi_params_catching(&s, 0) && g_catch_calls == 0);
    assert(midi_params_write_physical(&s, 0, 0) == 1);          /* the next button press applies at once */
    assert(midi_params_get(&s, 0) == 0);
    /* the default is pickup ON; turning it off mid-catch clears the catch and fires the hook */
    assert(midi_params_set_pickup(&s, 1, 1) == 0);
    assert(midi_params_set(&s, 1, 8000, MIDI_SRC_USB) == 1);
    assert(midi_params_catching(&s, 1) && g_catch_calls == 1);
    assert(midi_params_set_pickup(&s, 1, 0) == 0);
    assert(!midi_params_catching(&s, 1) && g_catch_calls == 2);
    assert(midi_params_write_physical(&s, 1, 100) == 1);
}

static void resolution_tests(void)
{
    struct midi_params s;
    struct log l;
    init(&s, &l, 1);
    assert(bind(&s, MIDI_TGT_CC14, 0, 24, 0) == 0);
    send(&s, MIDI_SRC_USB, 0xb0, 24, 64);
    assert(l.changes == 1 && l.value == 8256 && l.res == 7);
    send(&s, MIDI_SRC_USB, 0xb0, 56, 0);
    assert(l.changes == 2 && l.value == 8192 && l.res == 14);
    send(&s, MIDI_SRC_USB, 0xb0, 56, 64);
    assert(l.changes == 3 && l.value == 8256 && l.res == 14);
    send(&s, MIDI_SRC_USB, 0xb0, 24, 65);
    assert(l.changes == 4 && l.value == 8384 && l.res == 14); /* retained LSB */
    /* A lone MSB and explicit pair can have equal values: resolution still changes meaning. */
    send(&s, MIDI_SRC_BLE, 0xb0, 24, 64);
    assert(l.changes == 5 && l.value == 8256 && l.res == 7);
    send(&s, MIDI_SRC_BLE, 0xb0, 56, 64);
    assert(l.changes == 6 && l.value == 8256 && l.res == 14);
    send(&s, MIDI_SRC_BLE, 0xb0, 24, 64);
    assert(l.changes == 6); /* same value AND resolution */
    /* LSB first, then MSB, also reports 14 bits. */
    send(&s, MIDI_SRC_TRS, 0xb0, 56, 64);
    assert(l.changes == 7 && l.value == 64 && l.res == 14);
    send(&s, MIDI_SRC_TRS, 0xb0, 24, 64);
    assert(l.changes == 8 && l.value == 8256 && l.res == 14);
    /* An ignored physical sample must not change stored resolution. */
    send(&s, MIDI_SRC_LOCAL, 0xb0, 24, 127);
    assert(l.changes == 8 && s.p[0].res == 14);
    send(&s, MIDI_SRC_LOCAL, 0xb0, 24, 64);
    assert(l.changes == 9 && l.value == 8256 && l.res == 7);
    assert(midi_params_set(&s, 0, 8256, MIDI_SRC_ENGINE) == 1);
    assert(l.changes == 10 && l.res == 14);
    assert(midi_params_write_physical(&s, 0, 8256) == 1);
    assert(l.changes == 10);
}

static void event_tests(void)
{
    struct midi_params s;
    struct log l;
    init(&s, &l, 1);
    struct midi_in_bind b = { { MIDI_TGT_PROGRAM, 0, MIDI_NUM_ANY }, 0, 1, 0, 3 };
    assert(midi_params_bind(&s, &b) == 0);
    /* PC 0 is the very first message; both it and its repeat must be delivered. */
    for (unsigned i = 1; i <= 2; ++i) {
        send(&s, MIDI_SRC_USB, 0xc0, 0, 255);
        assert(l.changes == i && l.param == 0 && l.value == 0 && l.res == 7 && l.source == MIDI_SRC_USB);
    }
    for (unsigned i = 3; i <= 4; ++i) {
        send(&s, MIDI_SRC_USB, 0xc0, 2, 0);
        assert(l.changes == i && l.value == 2);
    }
    send(&s, MIDI_SRC_USB, 0xc0, 3, 0); /* inclusive upper bound */
    assert(l.changes == 5 && l.value == 3);
    for (unsigned i = 4; i < 128; ++i) send(&s, MIDI_SRC_USB, 0xc0, (uint8_t)i, 0);
    send(&s, MIDI_SRC_USB, 0xc1, 2, 0); /* wrong channel */
    assert(l.changes == 5 && l.catches == 0 && !midi_params_catching(&s, 0));
    assert(midi_params_get(&s, 0) == 0); /* events do not become state */
    /* Event delivery bypasses even an existing catch and leaves it untouched. */
    assert(midi_params_set(&s, 0, 9000, MIDI_SRC_ENGINE) == 1);
    struct midi_param_state before = s.p[0];
    unsigned changes = l.changes, catches = l.catches;
    for (uint8_t source = MIDI_SRC_LOCAL; source < MIDI_SRC_COUNT; ++source) {
        send(&s, source, 0xc0, 2, 0);
        assert(l.changes == ++changes && l.value == 2 && l.source == source && l.res == 7);
        assert(l.catches == catches && memcmp(&before, &s.p[0], sizeof(before)) == 0);
    }
    /* Reject invalid event configurations; a nonzero lower bound is inclusive too. */
    init(&s, &l, 1);
    b.event = 2; assert(midi_params_bind(&s, &b) == -1);
    b.event = 1; b.min = 4; assert(midi_params_bind(&s, &b) == -1);
    b.min = 0; b.max = 128; assert(midi_params_bind(&s, &b) == -1);
    b.t.kind = MIDI_TGT_CC; b.t.number = 102; b.max = 16384;
    assert(midi_params_bind(&s, &b) == -1);
    b.t = (struct midi_target){ MIDI_TGT_PROGRAM, 0, MIDI_NUM_ANY };
    b.min = 2; b.max = 3;
    assert(midi_params_bind(&s, &b) == 0);
    send(&s, MIDI_SRC_LOCAL, 0xc0, 1, 0); assert(l.changes == 0);
    send(&s, MIDI_SRC_LOCAL, 0xc0, 2, 0); assert(l.changes == 1 && l.value == 2);
    assert(bind(&s, MIDI_TGT_PROGRAM, 0, 4, 0) == -1); /* ranges do not relax overlap rules */
    init(&s, &l, 1);
    assert(midi_params_bind(&s, &b) == 0);
    s.changed = 0;
    send(&s, MIDI_SRC_USB, 0xc0, 2, 0); /* null callback is safe */
    assert(l.changes == 0 && l.catches == 0 && midi_params_get(&s, 0) == 0);
}

int main(void)
{
    resolution_tests(); event_tests(); no_pickup_tests();
    mixer_tests(); binding_tests(); arbiter_tests(); stream_tests();
    end_to_end(); invalid_tests(); fuzz_tests();
    puts("all midi_params tests passed (5000000 fuzz messages)");
    return 0;
}
