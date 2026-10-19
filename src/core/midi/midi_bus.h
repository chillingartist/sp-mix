/* Copyright (c) 2026 Ryan Gilmore. MIT License, see LICENSE. */
#ifndef MIDI_BUS_H
#define MIDI_BUS_H
#include <stdint.h>
#include "midi_msg.h"

/* midi_bus.h — routing. Every message is published with the SOURCE that made it; each sink says which sources it hears and which
 * source it is the OUTPUT of, and a message is never delivered to the sink that represents its own source (no echo, by construction).
 * Portable C99, no Zephyr, no dynamic memory.
 *
 * Today feldd hard-wires this: midi_out_send fans a message to TRS + USB + BLE and midi_out_thru forwards USB-in to TRS / BLE. The bus is that
 * fan-out as data, plus the internal sinks (the parameter bindings, a monitor) that feldd has no way to express. */

enum midi_source {
    MIDI_SRC_NONE   = 0,
    MIDI_SRC_LOCAL  = 1,    /* the device's own controls (and the loop-back of an engine's on-device controls) */
    MIDI_SRC_USB    = 2,    /* USB-MIDI in */
    MIDI_SRC_BLE    = 3,    /* BLE-MIDI in */
    MIDI_SRC_TRS    = 4,    /* TRS in (no such hardware today; reserved) */
    MIDI_SRC_ENGINE = 5,    /* an internal engine or generator (an arpeggiator, a scene recall) */
    MIDI_SRC_RSV6   = 6,
    MIDI_SRC_RSV7   = 7
};
#define MIDI_SRC_COUNT   8
#define MIDI_SRC_BIT(s)  ((uint8_t)(1u << (s)))
#define MIDI_SRC_ALL     0xFEu            /* every source except NONE */

/* A sink that wants the source (the parameter bindings do). */
typedef void (*midi_bus_fn)(const struct midi_msg *m, uint8_t source, void *ctx);

#define MIDI_BUS_MAX_SINKS 8

struct midi_bus_sink {
    midi_bus_fn   fn;           /* exactly one of fn / legacy is set */
    midi_sink_fn  legacy;       /* a plain feldd sink: (msg, ctx), the source is not passed */
    void         *ctx;
    uint8_t       accept;       /* bitmask of MIDI_SRC_BIT(s): the sources this sink hears */
    uint8_t       represents;   /* the source this sink is the output of (MIDI_SRC_NONE if none): it never receives that source's messages */
};

struct midi_bus {
    struct midi_bus_sink sink[MIDI_BUS_MAX_SINKS];
    uint8_t  disabled[MIDI_BUS_MAX_SINKS][MIDI_SRC_COUNT]; /* zero defaults to accept; atomic byte access */
    uint8_t  count;
    uint32_t published;         /* publish calls accepted */
    uint32_t delivered;         /* sink calls made */
    uint32_t suppressed;        /* sink calls skipped because the sink represents the message's own source (the no-echo rule) */
    uint32_t rejected;          /* publish calls refused: source 0 or above 7, or a null message */
};

void midi_bus_init(struct midi_bus *b);

/* Register a sink at start-up (the table is not locked: add every sink before the first publish). Returns the index, or -1 when the table is
 * full, the function is null, or `represents` is above 7. */
int midi_bus_add(struct midi_bus *b, midi_bus_fn fn, void *ctx, uint8_t accept, uint8_t represents);
int midi_bus_add_legacy(struct midi_bus *b, midi_sink_fn fn, void *ctx, uint8_t accept, uint8_t represents);

/* Routes only enable sources in accept; invalid indices/sources are ignored. No echo still applies.
 * Route accesses and diagnostic increments are relaxed atomics; registration/init
 * must finish before publishers start. Counters are individually atomic, not a snapshot. */
void midi_bus_set_route(struct midi_bus *b, unsigned sink_idx, uint8_t source, int on);

/* Deliver in registration order to enabled routes, excluding the source's own sink.
 * Concurrent publishers require context-safe sinks; no locks surround callbacks.
 * Null messages and invalid sources are counted and rejected. No callback re-entry. */
void midi_bus_publish(struct midi_bus *b, uint8_t source, const struct midi_msg *m);
/* As publish, restricted to sink indices whose bits are set. Does not change
 * persistent routes; accept, disabled and no-echo rules still apply. */
void midi_bus_publish_to(struct midi_bus *b, uint8_t source, const struct midi_msg *m, uint8_t sinks);
#endif
