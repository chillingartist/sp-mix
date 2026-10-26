/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ryan Gilmore
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>

#include "ctrl.h"
#include "midi/midi_bus.h"
#include "midi/usb_rt_parse.h"
#include "midi_in.h"
#include "midi_map.h"
#include "ui.h"
#include "usb_midi1.h"

/* A full-speed bulk packet is 16 events; a 2 ms tick sees a few at most */
#define MIDI_Q_LEN	64

K_MSGQ_DEFINE(midi_q, sizeof(struct midi_msg), MIDI_Q_LEN, 4);

static atomic_t packets;
static atomic_t messages;
static atomic_t dropped;
static uint32_t applied;

static struct midi_params params;
static struct sp1m_midi_context ctx;
static struct ui *ui;
static struct ctrl *ctrl;
static bool started;

/* USB thread: decode and queue, nothing else */
static void rx(const uint8_t *pkts, size_t len, void *user)
{
	ARG_UNUSED(user);

	for (size_t i = 0; i + 4 <= len; i += 4) {
		uint8_t b[3] = { 0 };
		uint8_t n = usb_midi_extract_voice(&pkts[i], b);

		atomic_inc(&packets);
		if (n == 0) {
			continue;
		}

		const struct midi_msg m = { .status = b[0], .d1 = b[1], .d2 = b[2], .len = n };

		if (k_msgq_put(&midi_q, &m, K_NO_WAIT) == 0) {
			atomic_inc(&messages);
		} else {
			atomic_inc(&dropped);
		}
	}
}

void midi_in_init(void)
{
	usb_midi1_set_rx(rx, NULL);
}

void midi_in_start(struct ui *u, const struct ui_in *in, struct ctrl *c)
{
	ui = u;
	ctrl = c;
	ctx.ui = u;
	ctx.in = in;
	sp1m_midi_params_init(&params, CONFIG_SP1M_MIDI_CHANNEL - 1);
	params.changed = sp1m_midi_changed;
	params.ctx = &ctx;
	started = true;
}

void midi_in_poll(void)
{
	struct midi_msg m;

	if (!started) {
		return;
	}
	/* The mixer, or the DAW's side in controller mode */
	ctx.ui = ctrl_ui(ctrl, ui);
	/* The state as the last tick left it: faders and buttons included */
	sp1m_midi_sync(&params, &ctx.ui->st.current);
	while (k_msgq_get(&midi_q, &m, K_NO_WAIT) == 0) {
		if (!ctrl_filter_in(ctrl, &m, ctx.in->now_ms)) {
			continue;
		}
		midi_params_on_msg(&m, MIDI_SRC_USB, &params);
		applied++;
	}
}

void midi_out_flush(void)
{
	struct midi_msg m;

	if (!started) {
		return;
	}
	while (ctrl_take(ctrl, &m)) {
		/* USB-MIDI 1.0 event packet, cable 0: CIN = the status nibble */
		const uint8_t pkt[4] = { (uint8_t)(m.status >> 4), m.status, m.d1, m.d2 };

		usb_midi1_send(pkt);
	}
}

void midi_in_get_stats(struct midi_in_stats *out)
{
	out->packets = (uint32_t)atomic_get(&packets);
	out->messages = (uint32_t)atomic_get(&messages);
	out->dropped = (uint32_t)atomic_get(&dropped);
	out->applied = applied;
	out->ignored = ctrl != NULL ? ctrl->ignored : 0;
	out->sent = ctrl != NULL ? ctrl->sent : 0;
	out->overflow = ctrl != NULL ? ctrl->overflow : 0;
}
