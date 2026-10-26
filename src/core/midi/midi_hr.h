/* Copyright (c) 2026 Ryan Gilmore. MIT License, see LICENSE. */
#ifndef MIDI_HR_H
#define MIDI_HR_H
#include <stdint.h>
#include "midi_msg.h"

/* midi_hr.h — high-resolution MIDI on top of the 7-bit midi_msg stream: 14-bit controllers (CC n / n+32), NRPN and
 * RPN, and pitch bend. Portable C99: no Zephyr, no dynamic memory, no floating point, no I/O, no globals. Host-tested.
 *
 * A VALUE IS ALWAYS 14 BITS (0..16383). A source that only had 7 bits is widened and says so in `res`. */

#define MIDI_HR_MAX 16383u

enum midi_hr_kind {
    MIDI_HR_CC14 = 1,       /* number = the MSB controller 0..31; value = (MSB << 7) | LSB */
    MIDI_HR_NRPN,           /* number = the parameter 0..16383; value = the data */
    MIDI_HR_RPN,            /* as NRPN, a registered parameter */
    MIDI_HR_PITCH_BEND      /* number = 0; value = the bend, 8192 = centre */
};

struct midi_hr_event {
    uint8_t  kind;          /* enum midi_hr_kind */
    uint8_t  channel;       /* 0..15 */
    uint8_t  res;           /* 7 or 14: the significant bits of `value` (an MSB-only sender is res 7) */
    uint16_t number;
    uint16_t value;         /* 0..16383 */
};

/* 7 to 14 bits so that 0 -> 0 and 127 -> 16383 (full scale stays full scale), rounding to nearest. */
static inline uint16_t midi_widen7(uint8_t v7)
{
    return (uint16_t)(((uint32_t)(v7 & 0x7Fu) * MIDI_HR_MAX + 63u) / 127u);
}
/* 14 to 7 bits, rounding to nearest, clamped; midi_narrow7(midi_widen7(x)) == x for every x in 0..127. */
static inline uint8_t midi_narrow7(uint16_t v14)
{
    if (v14 > MIDI_HR_MAX) v14 = (uint16_t)MIDI_HR_MAX;
    return (uint8_t)(((uint32_t)v14 * 127u + 8191u) / MIDI_HR_MAX);
}

/* ------------------------------ assembler: midi_msg stream -> hi-res events ------------------------------ */

struct midi_hr_asm {
    struct { uint8_t msb, lsb, seen_lsb; } c14[16][32];     /* per channel, per MSB controller */
    struct {
        uint8_t sel_msb, sel_lsb, have_msb, have_lsb;       /* the selected parameter number */
        uint8_t is_rpn;
        uint8_t data_msb, data_lsb, seen_data_lsb;
        uint8_t selected;                                   /* a complete, non-null parameter is selected */
    } nrpn[16];
};

void midi_hr_asm_init(struct midi_hr_asm *a);

/* Offer one message from ANY source. Returns 1 and fills *out when this message completes or updates a hi-res value:
 *  - 14-bit CC. A control change with controller 0..31 (an MSB) or 32..63 (an LSB) updates that pair.
 *      LSB arrives:  value = (msb << 7) | lsb, res 14; the controller is now known to be 14-bit (seen_lsb).
 *      MSB arrives, an LSB was seen before: value = (msb << 7) | the retained lsb, res 14.
 *      MSB arrives, no LSB ever seen (a 7-bit sender): value = midi_widen7(msb), res 7.
 *    The pair is assembled for EVERY controller below 64; a consumer that does not use the pair ignores the event.
 *    EXCEPT controllers 6 and 38 while an NRPN / RPN parameter is selected on that channel: they are then the data entry (below).
 *  - NRPN / RPN. Select with CC 99 / 98 (NRPN) or 101 / 100 (RPN); data with CC 6 (MSB) and 38 (LSB); CC 96 / 97 increment / decrement the
 *    14-bit data value by 1 (clamped). A select needs both halves (either order). A half of the same kind keeps the other half, so a sender that
 *    re-sends only the changed half works; the other kind forgets both. Selecting resets the data registers and produces nothing; RPN 127 / 127
 *    (null) deselects (the expander can still SEND a null: it is a valid message, the assembler just reports no event for it). A data MSB alone: value = midi_widen7(msb), res 7; with an LSB (before or after): (msb << 7) | lsb, res 14.
 *    Nothing is produced for data entry while no complete parameter is selected.
 *  - Pitch bend (status 0xE0..0xEF, d1 = LSB, d2 = MSB): kind PITCH_BEND, res 14.

 * An MSB-only (res 7) event leaves any LSB the receiver already holds in place (the expander never sends an LSB for it), so after a 14-bit history a res 7
 * event is "the MSB changed", not necessarily the widened value.
 * Anything else (notes, other controllers, program change, real-time, a malformed message) returns 0. The assembler only OBSERVES: the caller
 * still owns the message and forwards or uses it as the plain 7-bit message it is. Data bytes with bit 7 set make the message ignored (0). */
int midi_hr_asm_feed(struct midi_hr_asm *a, const struct midi_msg *m, struct midi_hr_event *out);

/* ------------------------------ expander: hi-res event -> the minimal midi_msg sequence ------------------------------ */

struct midi_hr_enc {
    struct { uint8_t msb, lsb, valid_msb, valid_lsb; } c14[16][32];   /* the last halves sent */
    struct { uint16_t param; uint8_t valid, is_rpn, data_msb, data_lsb, valid_msb, valid_lsb; } nrpn[16];
};

#define MIDI_HR_EXPAND_MAX 6    /* the most messages one event can expand to (NRPN: 2 select + 2 data) is 4; headroom */

void midi_hr_enc_init(struct midi_hr_enc *e);
void midi_hr_enc_forget(struct midi_hr_enc *e);   /* forget what was sent (after a reconnect or a pause): the next event sends everything */

/* Expand one event into out[0 .. n-1] (cap >= MIDI_HR_EXPAND_MAX) and return n; 0 means "the receiver already holds this value".
 *  - CC14 (number 0..31 except 6: controller 6 / 38 is the data-entry pair, reserved for NRPN / RPN): CC number (MSB) then CC number+32 (LSB), each only if it differs from what this encoder last sent on that channel
 *    and controller (MSB first). A res 7 event sends the MSB only (narrowed with midi_narrow7) and leaves the LSB untouched.
 *  - PITCH_BEND: one message (d1 = LSB, d2 = MSB), always sent (a bend is not de-duplicated); a res 7 event is widened first.
 *  - NRPN / RPN: (an RPN null, number 16383, sends only the select: it deselects and no data follows) the parameter select (CC 99 then 98, or 101 then 100) only if it differs from the last select on that channel (or the kind
 *    changed), then data MSB (CC 6) and LSB (CC 38) each only if changed; a res 7 event sends CC 6 only.
 * Every message has len 3. A channel above 15, a number out of range for the kind, a value above 16383, an unknown kind or a cap too small returns 0
 * and sends nothing (and leaves the cache untouched). The ORDER of the sequence is part of the contract: it must be sent in order and as a GROUP. */
int midi_hr_expand(struct midi_hr_enc *e, const struct midi_hr_event *ev, struct midi_msg *out, int cap);

#endif
