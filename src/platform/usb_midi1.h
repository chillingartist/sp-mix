/* Copyright (c) 2026 Benjamin Reece. MIT License, see LICENSE. (Receive hook: Ryan Gilmore.) */
#ifndef USB_MIDI1_H
#define USB_MIDI1_H
#include <stddef.h>
#include <stdint.h>

/* Hand-rolled USB-MIDI 1.0 device_next class, one two-way port (the MIDI function
 * alongside CDC-ACM; the old USB-MIDI 2.0 function was dropped). A host binds
 * this and sees a real MIDI port with no mode toggle. The class is defined with
 * USBD_DEFINE_CLASS, so sample_usbd's usbd_register_all_classes picks it up
 * automatically at boot; there is no DT node or extra CONFIG to wire.
 *
 * Enqueue one 4-byte USB-MIDI 1.0 event packet (built by midi1_event) on our
 * bulk IN endpoint, to be flushed to the host. The send is non-blocking: on a
 * full TX ring (or before the host has enabled the interface) the packet is
 * dropped, never blocked — same graceful-drop posture as midi_out's USB sink.
 * Safe to call from the control loop. */
void usb_midi1_send(const uint8_t pkt[4]);

/* Host->device: called from the USB thread with each received buffer of 4-byte
 * USB-MIDI 1.0 event packets (len bytes; a trailing partial packet is the hook's
 * to ignore). Set it before USB starts; NULL drops what arrives. */
typedef void (*usb_midi1_rx_fn)(const uint8_t *pkts, size_t len, void *ctx);
void usb_midi1_set_rx(usb_midi1_rx_fn fn, void *ctx);

#endif
