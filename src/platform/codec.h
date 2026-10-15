/*
 * SPDX-License-Identifier: MIT
 *
 * CS42L42 (headphones, LRCLK master) and TAS2505 (speaker) over I2C.
 * Both receive the same I2S data line: 48 kHz, 24-bit slots in a 64 fs frame,
 * the bit clock comes from the 3.072 MHz oscillator.
 */

#ifndef SP1M_CODEC_H_
#define SP1M_CODEC_H_

#include <stdbool.h>

#include <stdint.h>

struct codec_status {
	bool cs42_answers;	/* the CS42L42 acknowledged I2C */
	bool cs42_ok;		/* CS42L42 fully set up */
	bool tas_ok;		/* TAS2505 fully set up */
	uint8_t cs42_step;	/* failed CS42L42 step: 1 clocks, 2 port, 3 HP; 0 none */
	uint8_t cs42_tries;	/* set-up attempts (a second one after a failure) */
	uint8_t tas_step;	/* failed TAS2505 write, 1-based; 0 none */
	uint8_t osc_stat;	/* 0x1109: 0x02 once SCLK clocks the chip, RCO off */
	uint8_t jack;		/* last 0x1b77: bit 7 = headphones plugged in */
	bool jack_valid;
	bool speaker_on;	/* what the TAS2505 was last set to */
	uint32_t i2c_errors;
};

/*
 * Oscillator on, both chips out of reset and configured. LRCLK runs after
 * this returns. Headphones and speaker start muted. A CS42L42 failure is
 * retried once with a longer reset; codec_get_status() says what failed.
 */
int codec_init(void);

/* Mute both outputs, power down, hold both chips in reset, oscillator off. */
void codec_power_down(void);

void codec_headphones_mute(bool mute);

/* Returns true once the speaker is in the requested state (false: retry). */
bool codec_speaker_enable(bool on);

/*
 * Jack sense through the CS42L42. 0 and *present set, or a negative error:
 * then keep the previous state.
 */
int codec_headphones_read(bool *present);

void codec_get_status(struct codec_status *out);

/* The CS42L42 is fully set up: the jack reading means something and the
 * headphones can play. Without it only the speaker can.
 */
bool codec_jack_usable(void);

/* Console diagnostics */
int codec_cs42_read(uint16_t reg, uint8_t *val);
int codec_tas_read(uint8_t page, uint8_t reg, uint8_t *val);

#endif /* SP1M_CODEC_H_ */
