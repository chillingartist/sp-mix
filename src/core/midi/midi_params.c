/* Copyright (c) 2026 Ryan Gilmore. MIT License, see LICENSE. */
#include "midi_params.h"
#include <string.h>

static uint16_t clamp14(uint16_t v)
{
    return v > MIDI_HR_MAX ? (uint16_t)MIDI_HR_MAX : v;
}

static int valid_id(const struct midi_params *s, uint16_t id)
{
    return s && id < s->nparams && id < MIDI_PARAMS_MAX;
}

void midi_params_init(struct midi_params *s, uint16_t nparams,
                      midi_param_changed_fn changed, midi_param_catch_fn catch_fn, void *ctx)
{
    if (!s) return;
    memset(s, 0, sizeof(*s));
    s->nparams = nparams > MIDI_PARAMS_MAX ? MIDI_PARAMS_MAX : nparams;
    for (unsigned i = 0; i < s->nparams; ++i) s->p[i].res = 14;
    s->margin = MIDI_PARAMS_DEFAULT_MARGIN;
    s->changed = changed;
    s->catch_fn = catch_fn;
    s->ctx = ctx;
    for (unsigned i = 0; i < MIDI_PARAMS_STREAMS; ++i) midi_hr_asm_init(&s->stream[i]);
}

void midi_params_set_margin(struct midi_params *s, uint16_t margin14)
{
    if (s) s->margin = clamp14(margin14);
}

static int parameter_kind(uint8_t kind)
{
    return kind == MIDI_TGT_NRPN || kind == MIDI_TGT_RPN;
}

static int reserved_cc(uint16_t n)
{
    return n == 6 || n == 38 || n == 96 || n == 97 || (n >= 98 && n <= 101);
}

static int overlaps(const struct midi_target *a, const struct midi_target *b)
{
    if (a->channel != b->channel && a->channel != MIDI_CH_ANY && b->channel != MIDI_CH_ANY) return 0;
    if (a->kind == b->kind)
        return a->number == b->number || (a->kind == MIDI_TGT_PROGRAM &&
               (a->number == MIDI_NUM_ANY || b->number == MIDI_NUM_ANY));
    if (a->kind == MIDI_TGT_CC14 && b->kind == MIDI_TGT_CC)
        return b->number == a->number || b->number == a->number + 32u;
    if (b->kind == MIDI_TGT_CC14 && a->kind == MIDI_TGT_CC) return overlaps(b, a);
    if (parameter_kind(a->kind)) {
        if (b->kind == MIDI_TGT_CC) return reserved_cc(b->number);
        if (b->kind == MIDI_TGT_CC14) return reserved_cc(b->number) || reserved_cc((uint16_t)(b->number + 32u));
    }
    if (parameter_kind(b->kind) && (a->kind == MIDI_TGT_CC || a->kind == MIDI_TGT_CC14)) return overlaps(b, a);
    return 0;
}

int midi_params_bind(struct midi_params *s, const struct midi_in_bind *b)
{
    if (!s || !b || !valid_id(s, b->param) || !midi_target_valid(&b->t, 1) ||
        s->nbinds >= MIDI_PARAMS_MAX_BINDS || b->event > 1 ||
        (b->event && (b->min > b->max || b->max > MIDI_HR_MAX ||
                      (b->t.kind == MIDI_TGT_PROGRAM && b->max > 127)))) return -1;
    for (unsigned i = 0; i < s->nbinds; ++i)
        if (overlaps(&s->bind[i].t, &b->t)) return -1;
    s->bind[s->nbinds++] = *b;
    return 0;
}

int midi_params_set_pickup(struct midi_params *s, uint16_t param, int enabled)
{
    if (!valid_id(s, param)) return -1;
    struct midi_param_state *p = &s->p[param];
    p->no_pickup = enabled ? 0 : 1;
    if (!enabled && p->catch_state) {
        p->catch_state = 0;
        if (s->catch_fn) s->catch_fn(param, 0, s->ctx);
    }
    return 0;
}

void midi_params_preset(struct midi_params *s, uint16_t param, uint16_t value14)
{
    if (valid_id(s, param)) {
        s->p[param].value = clamp14(value14);
        s->p[param].res = 14;
    }
}

uint16_t midi_params_get(const struct midi_params *s, uint16_t param)
{
    return valid_id(s, param) ? s->p[param].value : 0;
}

int midi_params_catching(const struct midi_params *s, uint16_t param)
{
    return valid_id(s, param) && s->p[param].catch_state != 0;
}

static int apply(struct midi_params *s, uint16_t param, uint16_t value, uint8_t source, uint8_t res)
{
    if (s->p[param].value == value && s->p[param].res == res) return 0;
    s->p[param].value = value;
    s->p[param].res = res;
    if (s->changed) s->changed(param, value, source, res, s->ctx);
    return 1;
}

static int set(struct midi_params *s, uint16_t param, uint16_t value14, uint8_t source, uint8_t res)
{
    if (!valid_id(s, param)) return -1;
    struct midi_param_state *p = &s->p[param];
    if (p->no_pickup) return apply(s, param, clamp14(value14), source, res);   /* a switch or state: nothing to catch */
    int was_catching = p->catch_state != 0;
    p->target = clamp14(value14);
    p->catch_state = 1;
    p->from_above = 0;
    if (!was_catching && s->catch_fn) s->catch_fn(param, 1, s->ctx);
    return apply(s, param, p->target, source, res);
}

static int write_physical(struct midi_params *s, uint16_t param, uint16_t value14, uint8_t res)
{
    if (!valid_id(s, param)) return -1;
    struct midi_param_state *p = &s->p[param];
    uint16_t v = clamp14(value14);
    if (p->catch_state) {
        int32_t delta = (int32_t)v - p->target;
        if (p->catch_state == 1) {
            p->from_above = delta > 0;
            p->catch_state = 2;
        }
        if (p->from_above ? delta > (int32_t)s->margin : delta < -(int32_t)s->margin) return 0;
        p->catch_state = 0;
        if (s->catch_fn) s->catch_fn(param, 0, s->ctx);
    }
    apply(s, param, v, MIDI_SRC_LOCAL, res);
    return 1;
}

int midi_params_set(struct midi_params *s, uint16_t param, uint16_t value14, uint8_t source)
{
    return set(s, param, value14, source, 14);
}

int midi_params_write_physical(struct midi_params *s, uint16_t param, uint16_t value14)
{
    return write_physical(s, param, value14, 14);
}

void midi_params_on_msg(const struct midi_msg *m, uint8_t source, void *ctx)
{
    struct midi_params *s = ctx;
    if (!s || !m || source == 0 || source >= MIDI_SRC_COUNT) return;
    uint8_t type = m->status & 0xf0u, channel = m->status & 15u;
    if ((type != 0xb0u && type != 0xc0u && type != 0xe0u) ||
        m->len != (type == 0xc0u ? 2 : 3) || m->d1 > 127 ||
        (type != 0xc0u && m->d2 > 127)) return;
    struct midi_hr_event ev;
    int have_hr = source <= MIDI_SRC_TRS && midi_hr_asm_feed(&s->stream[source - 1u], m, &ev);
    for (unsigned i = 0; i < s->nbinds && i < MIDI_PARAMS_MAX_BINDS; ++i) {
        const struct midi_in_bind *b = &s->bind[i];
        if (b->t.channel != MIDI_CH_ANY && b->t.channel != channel) continue;
        uint16_t value;
        uint8_t res = 7;
        if (b->t.kind == MIDI_TGT_CC) {
            if (type != 0xb0u || b->t.number != m->d1) continue;
            value = midi_widen7(m->d2);
        } else if (b->t.kind == MIDI_TGT_PROGRAM) {
            if (type != 0xc0u || (b->t.number != MIDI_NUM_ANY && b->t.number != m->d1)) continue;
            value = m->d1;
        } else {
            if (!have_hr || b->t.number != ev.number) continue;
            if (!((b->t.kind == MIDI_TGT_CC14 && ev.kind == MIDI_HR_CC14) ||
                  (b->t.kind == MIDI_TGT_NRPN && ev.kind == MIDI_HR_NRPN) ||
                  (b->t.kind == MIDI_TGT_RPN && ev.kind == MIDI_HR_RPN) ||
                  (b->t.kind == MIDI_TGT_PITCH_BEND && ev.kind == MIDI_HR_PITCH_BEND))) continue;
            value = ev.value;
            res = ev.res;
        }
        if (b->event) {
            if (value >= b->min && value <= b->max && s->changed)
                s->changed(b->param, value, source, res, s->ctx);
        } else if (source == MIDI_SRC_LOCAL) write_physical(s, b->param, value, res);
        else set(s, b->param, value, source, res);
        return;
    }
}
