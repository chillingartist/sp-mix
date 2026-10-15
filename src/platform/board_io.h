/*
 * SPDX-License-Identifier: MIT
 *
 * SP-1 GPIO level control: oscillator, codec resets, charger, eMMC, Bluetooth
 * module, button supply and the FUNCTION button.
 */

#ifndef SP1M_BOARD_IO_H_
#define SP1M_BOARD_IO_H_

#include <stdbool.h>

/* Puts every controlled pin into its safe default. Call first in main(). */
int board_io_init(void);

void board_osc_enable(bool on);
void board_cs42_reset(bool assert);
void board_tas_reset(bool assert);

/*
 * Both codecs into reset and the oscillator off right now, by register
 * writes only: callable from any context, also zero latency interrupts.
 * For resets that cannot mute first (stall catcher, crash, watchdog); a
 * click is better than whatever a running codec makes of floating pins.
 */
void board_codecs_kill(void);
void board_btn_supply(bool on);
void board_charger_enable(bool on);

bool board_fn_pressed(void);
bool board_charging(void);
/* VBUS present (USB host or charger plugged in) */
bool board_usb_power(void);

/*
 * Before SYSTEM_OFF: LEDs, button supply, oscillator, codecs and eMMC off,
 * Bluetooth module in reset, FUNCTION armed as the wake-up source.
 */
void board_prepare_system_off(void);

#endif /* SP1M_BOARD_IO_H_ */
