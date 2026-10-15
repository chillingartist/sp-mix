/*
 * SPDX-License-Identifier: MIT
 *
 * The 8 front LEDs on PWM0/PWM1. Index 0-3: track LEDs, 4-7: playback LEDs.
 */

#ifndef SP1M_LEDS_H_
#define SP1M_LEDS_H_

#include <stdint.h>

#define LEDS_COUNT 8

int leds_init(void);

/* Perceptual brightness 0..255 (gamma corrected). */
void leds_set(const uint8_t level[LEDS_COUNT]);
void leds_all(uint8_t level);

#endif /* SP1M_LEDS_H_ */
