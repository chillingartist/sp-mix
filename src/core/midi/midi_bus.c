/* Copyright (c) 2026 Ryan Gilmore. MIT License, see LICENSE. */
#include "midi_bus.h"
#include <string.h>

void midi_bus_init(struct midi_bus *b)
{
    if (b) memset(b, 0, sizeof(*b));
}

static int add(struct midi_bus *b, midi_bus_fn fn, midi_sink_fn legacy,
               void *ctx, uint8_t accept, uint8_t represents)
{
    if (!b || (!fn && !legacy) || represents >= MIDI_SRC_COUNT ||
        b->count >= MIDI_BUS_MAX_SINKS) return -1;
    int index = b->count++;
    b->sink[index] = (struct midi_bus_sink){ fn, legacy, ctx, accept, represents };
    return index;
}

int midi_bus_add(struct midi_bus *b, midi_bus_fn fn, void *ctx,
                 uint8_t accept, uint8_t represents)
{
    return add(b, fn, 0, ctx, accept, represents);
}

int midi_bus_add_legacy(struct midi_bus *b, midi_sink_fn fn, void *ctx,
                        uint8_t accept, uint8_t represents)
{
    return add(b, 0, fn, ctx, accept, represents);
}

void midi_bus_set_route(struct midi_bus *b, unsigned sink_idx, uint8_t source, int on)
{
    if (!b || sink_idx >= b->count || sink_idx >= MIDI_BUS_MAX_SINKS ||
        source == MIDI_SRC_NONE || source >= MIDI_SRC_COUNT) return;
    __atomic_store_n(&b->disabled[sink_idx][source], !on, __ATOMIC_RELAXED);
}

void midi_bus_publish_to(struct midi_bus *b, uint8_t source, const struct midi_msg *m, uint8_t sinks)
{
    if (!b) return;
    if (!m || source == MIDI_SRC_NONE || source >= MIDI_SRC_COUNT) {
        __atomic_fetch_add(&b->rejected, 1u, __ATOMIC_RELAXED);
        return;
    }
    __atomic_fetch_add(&b->published, 1u, __ATOMIC_RELAXED);
    for (unsigned i = 0; i < b->count && i < MIDI_BUS_MAX_SINKS; ++i) {
        if (!(sinks & (1u << i))) continue;
        const struct midi_bus_sink *s = &b->sink[i];
        if (!(s->accept & MIDI_SRC_BIT(source)) ||
            __atomic_load_n(&b->disabled[i][source], __ATOMIC_RELAXED)) continue;
        if (s->represents == source) {
            __atomic_fetch_add(&b->suppressed, 1u, __ATOMIC_RELAXED);
            continue;
        }
        if (s->fn) s->fn(m, source, s->ctx);
        else if (s->legacy) s->legacy(m, s->ctx);
        else continue;
        __atomic_fetch_add(&b->delivered, 1u, __ATOMIC_RELAXED);
    }
}

void midi_bus_publish(struct midi_bus *b, uint8_t source, const struct midi_msg *m)
{
    midi_bus_publish_to(b, source, m, UINT8_MAX);
}
