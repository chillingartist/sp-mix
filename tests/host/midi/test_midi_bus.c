/* Copyright (c) 2026 Ryan Gilmore. MIT License, see LICENSE. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "midi_bus.h"

struct log { int n, order[64]; uint8_t source[64]; struct midi_msg msg[64]; };
struct sink { struct log *log; int id; };
static void record(const struct midi_msg *m, uint8_t source, void *ctx)
{
    struct sink *s = ctx;
    int n = s->log->n++;
    assert(n < 64);
    s->log->order[n] = s->id;
    s->log->source[n] = source;
    s->log->msg[n] = *m;
}
static void legacy(const struct midi_msg *m, void *ctx) { record(m, 0, ctx); }

int main(void)
{
    struct midi_bus b;
    struct log log = {0};
    struct sink a = { &log, 10 }, c = { &log, 20 }, d = { &log, 30 };
    struct midi_msg m = { 0xb0, 20, 93, 3 };
    midi_bus_init(&b);
    assert(b.count == 0 && b.published == 0 && b.rejected == 0 && b.delivered == 0 && b.suppressed == 0);
    assert(midi_bus_add(&b, 0, &a, MIDI_SRC_ALL, 0) == -1);
    assert(midi_bus_add_legacy(&b, 0, &a, MIDI_SRC_ALL, 0) == -1);
    for (int i = 8; i < 256; ++i) {
        assert(midi_bus_add(&b, record, &a, MIDI_SRC_ALL, (uint8_t)i) == -1);
        assert(midi_bus_add_legacy(&b, legacy, &a, MIDI_SRC_ALL, (uint8_t)i) == -1);
    }
    assert(b.count == 0);
    assert(midi_bus_add(&b, record, &a, MIDI_SRC_ALL, 0) == 0);
    assert(midi_bus_add_legacy(&b, legacy, &c, MIDI_SRC_ALL, MIDI_SRC_USB) == 1);
    assert(midi_bus_add(&b, record, &d, MIDI_SRC_BIT(MIDI_SRC_LOCAL), 0) == 2);
    assert(midi_bus_add(&b, record, &a, MIDI_SRC_ALL, 0) == 3);
    midi_bus_publish(&b, MIDI_SRC_USB, &m);
    assert(log.n == 2 && log.order[0] == 10 && log.order[1] == 10);
    assert(log.source[0] == MIDI_SRC_USB && log.source[1] == MIDI_SRC_USB);
    midi_bus_publish(&b, MIDI_SRC_LOCAL, &m);
    assert(log.n == 6 && log.order[2] == 10 && log.order[3] == 20 && log.order[4] == 30 && log.order[5] == 10);
    assert(log.source[3] == 0 && log.source[4] == MIDI_SRC_LOCAL);
    for (int i = 0; i < log.n; ++i) assert(memcmp(&log.msg[i], &m, sizeof(m)) == 0);
    /* Suppression is a skipped eligible sink call, not an accept-mask rejection. */
    assert(midi_bus_add(&b, record, &a, 0, MIDI_SRC_USB) == 4);
    midi_bus_publish(&b, MIDI_SRC_USB, &m);
    assert(b.published == 3 && b.delivered == 8 && b.suppressed == 2 && b.rejected == 0);
    for (int i = 0; i < 256; ++i)
        if (i == 0 || i > 7) midi_bus_publish(&b, (uint8_t)i, &m);
    midi_bus_publish(&b, MIDI_SRC_LOCAL, 0);
    assert(b.rejected == 250 && log.n == 8 && b.published == 3);
    for (int i = 5; i < MIDI_BUS_MAX_SINKS; ++i)
        assert(midi_bus_add_legacy(&b, legacy, &c, MIDI_SRC_ALL, 0) == i);
    assert(midi_bus_add(&b, record, &a, MIDI_SRC_ALL, 0) == -1);
    assert(midi_bus_add_legacy(&b, legacy, &c, MIDI_SRC_ALL, 0) == -1);
    assert(b.count == MIDI_BUS_MAX_SINKS);
    midi_bus_publish(&b, 7, &m);
    assert(b.published == 4 && b.delivered == 14 && b.suppressed == 2);
    midi_bus_init(0); midi_bus_publish(0, 255, 0);
    assert(midi_bus_add(0, record, &a, 255, 0) == -1);
    puts("all midi_bus tests passed");
    return 0;
}
