/*
 * SPDX-License-Identifier: MIT
 *
 * Control loop (main thread, 500 Hz): inputs -> interaction state machine ->
 * mixer parameters, LEDs, speaker, persistence, power and recovery.
 */

#ifndef SP1M_CONTROL_H_
#define SP1M_CONTROL_H_

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/toolchain.h>

#include "inputs.h"
#include "scene.h"

enum ctrl_cmd_type {
	CTRL_SCENE_SAVE,
	CTRL_SCENE_LOAD,
	CTRL_SPEAKER,		/* arg: enum spk_mode */
	CTRL_CHARGE,		/* arg: 1 = charging on */
	CTRL_BOOTLOADER,
	CTRL_POWER_OFF,
	CTRL_SELFTEST,		/* arg: 0 = stall the loop, 1 = stop the watchdog */
	CTRL_MASTER,		/* arg: master volume in dB */
	CTRL_SPEAKER_FORCE,	/* arg: 1 = speaker on whatever the jack says */
	CTRL_REBOOT,		/* silence everything, then reboot */
};

struct ctrl_cmd {
	uint8_t type;
	int8_t arg;
};

struct ctrl_status {
	struct raw_inputs raw;
	uint32_t buttons;
	uint16_t fader[SP1M_TRACKS];
	struct scene cur;
	uint8_t slot_valid;
	bool scene_layer;
	bool hp_present;
	bool hp_known;		/* the jack has been read successfully */
	bool speaker_on;	/* actual state of the speaker amplifier */
	bool speaker_forced;	/* console `spk on` */
	bool charging;
	bool charge_off;
	bool dirty;
	uint32_t store_writes;
	uint32_t store_errors;
	bool pickup_pending[SP1M_TRACKS];
};

/* Runs the control loop; never returns. @loaded may be NULL. */
FUNC_NORETURN void control_run(const struct persist *loaded);

/* Queue a command from another thread (serial console). */
int control_post(const struct ctrl_cmd *cmd);

void control_get_status(struct ctrl_status *out);

#endif /* SP1M_CONTROL_H_ */
