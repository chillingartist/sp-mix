/* Copyright (c) 2026 Benjamin Reece. MIT License, see LICENSE. (struct and typedef are feldd's own.) */
#ifndef MIDI_MSG_H
#define MIDI_MSG_H
#include <stdint.h>

/* midi_msg.h — the wire-level MIDI message and the sink every producer hands it to.
 *
 * Moved out of mapping.h unchanged (the struct and the typedef are feldd's own, Benjamin Reece, MIT) so that
 * pure MIDI modules (midi_hr, midi_bus, ...) can use them without pulling in the profile. `len` is 2 or 3 for a
 * channel-voice message and 1 for a system real-time byte. A message carries 7-bit data bytes only: anything
 * finer is a midi_hr event expanded into several of these. */
struct midi_msg { uint8_t status, d1, d2; uint8_t len; };  /* len 2 or 3 */
typedef void (*midi_sink_fn)(const struct midi_msg *m, void *ctx);

#endif
