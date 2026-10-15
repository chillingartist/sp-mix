/*
 * SPDX-License-Identifier: MIT
 *
 * Register sequences:
 * - CS42L42: timknapen/SP-1-dev wiki (I2C page), CS42L42 datasheet DS1083F8
 *   section 5.1 power-up sequence. 24-bit ASP channels.
 * - TAS2505: SLAU472C; PLL locked to the bit clock as used by the
 *   sp1-tape-looper project, with 24-bit words and the (L+R)/2 DAC input.
 * SP-1-dev: Copyright (c) 2026 Tim Knapen; sp1-tape-looper: Copyright (c)
 * 2026 chattock; both MIT, see THIRD_PARTY_NOTICES.md.
 */

#include <errno.h>

#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>

#include "board_io.h"
#include "codec.h"
#include "safety.h"

#define CS42_ADDR	0x48
#define TAS_ADDR	0x18

/* TAS2505 P0/R63: DAC powered, input (L+R)/2, soft stepping */
#define TAS_DAC_PATH_MIX	0xb0
/* Class-D driver gain, P1/R48: 0x00 mute, 0x10 +6 dB ... 0x40 +24 dB */
#define TAS_SPK_GAIN_ON		0x20
#define TAS_SPK_GAIN_MUTE	0x00
/* CS42L42 mixer input volume (1 dB steps, 0 = 0 dB, 0x3f mute): caps the
 * headphone level below the chip's maximum
 */
#define CS42_HP_MIX_VOL		CONFIG_SP1M_HP_ATTEN_DB

BUILD_ASSERT(CS42_HP_MIX_VOL < 0x3f, "0x3f mutes the mixer");

static const struct device *const i2c = DEVICE_DT_GET(DT_NODELABEL(i2c0));
static struct codec_status st;
/* Page select and register access are two transfers: one thread at a time
 * (the console reads registers while the control thread polls the jack).
 * Also covers st.
 */
static K_MUTEX_DEFINE(lock);

struct reg16 {
	uint16_t reg;
	uint8_t val;
};

/* CS42L42 registers are paged: register 0x00 selects the page. */
static int cs42_write(uint16_t reg, uint8_t val)
{
	uint8_t page[2] = { 0x00, (uint8_t)(reg >> 8) };
	uint8_t data[2] = { (uint8_t)(reg & 0xff), val };
	int err;

	k_mutex_lock(&lock, K_FOREVER);
	err = i2c_write(i2c, page, sizeof(page), CS42_ADDR);
	if (!err) {
		err = i2c_write(i2c, data, sizeof(data), CS42_ADDR);
	}
	if (err) {
		st.i2c_errors++;
	} else {
		st.cs42_answers = true;
	}
	k_mutex_unlock(&lock);

	return err;
}

static int cs42_read(uint16_t reg, uint8_t *val)
{
	uint8_t page[2] = { 0x00, (uint8_t)(reg >> 8) };
	uint8_t r = (uint8_t)(reg & 0xff);
	int err;

	k_mutex_lock(&lock, K_FOREVER);
	err = i2c_write(i2c, page, sizeof(page), CS42_ADDR);
	if (!err) {
		err = i2c_write_read(i2c, CS42_ADDR, &r, 1, val, 1);
	}
	if (err) {
		st.i2c_errors++;
	}
	k_mutex_unlock(&lock);

	return err;
}

/* Stops at the first failure: a dead bus must not hold up the boot. */
static int cs42_write_seq(const struct reg16 *seq, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		if (cs42_write(seq[i].reg, seq[i].val) != 0) {
			return -EIO;
		}
	}

	return 0;
}

static int tas_write(uint8_t page, uint8_t reg, uint8_t val)
{
	uint8_t p[2] = { 0x00, page };
	uint8_t d[2] = { reg, val };
	int err;

	k_mutex_lock(&lock, K_FOREVER);
	err = i2c_write(i2c, p, sizeof(p), TAS_ADDR);
	if (!err) {
		err = i2c_write(i2c, d, sizeof(d), TAS_ADDR);
	}
	if (err) {
		st.i2c_errors++;
	}
	k_mutex_unlock(&lock);

	return err;
}

static int tas_read(uint8_t page, uint8_t reg, uint8_t *val)
{
	uint8_t p[2] = { 0x00, page };
	int err;

	k_mutex_lock(&lock, K_FOREVER);
	err = i2c_write(i2c, p, sizeof(p), TAS_ADDR);
	if (!err) {
		err = i2c_write_read(i2c, TAS_ADDR, &reg, 1, val, 1);
	}
	if (err) {
		st.i2c_errors++;
	}
	k_mutex_unlock(&lock);

	return err;
}

/* Before switching the internal MCLK to SCLK (datasheet 5.1 steps 3-5) */
static const struct reg16 cs42_clock_seq[] = {
	{ 0x1508, 0x10 },	/* PLL control 3 */
	{ 0x1504, 0x80 },	/* PLL division fractional */
	{ 0x1505, 0x3e },	/* PLL division integer */
	{ 0x150a, 0x7d },	/* PLL calibration ratio */
	{ 0x1009, 0x00 },	/* MCLK control */
	{ 0x1201, 0x01 },	/* MCLK source: PLL (SCLK x 62.5 / 16 = 12 MHz) */
	{ 0x120a, 0x01 },	/* input ASRC clock */
	{ 0x120b, 0x01 },	/* output ASRC clock */
	{ 0x1501, 0x01 },	/* PLL control 1 */
	{ 0x1107, 0x01 },	/* oscillator switch */
};

/* After the oscillator switch: serial port, routing, power up */
static const struct reg16 cs42_port_seq[] = {
	{ 0x1007, 0x13 },	/* serial port SRC control */
	{ 0x1203, 0x1f },	/* FSYNC pulse width */
	{ 0x1205, 0x3f },	/* FSYNC period: 64 SCLK */
	{ 0x1207, 0x34 },	/* ASP clock configuration 1: LRCLK master */
	{ 0x1208, 0x1a },	/* ASP frame configuration */
	{ 0x2a02, 0x02 },	/* ASP RX channel 1: 24 bit */
	{ 0x2a05, 0x42 },	/* ASP RX channel 2: phase, 24 bit */
	{ 0x2601, 0x4c },	/* SRC input sample rate */
	{ 0x2609, 0x4c },	/* SRC output sample rate */
	{ 0x2a01, 0x0c },	/* ASP receive enable */
	{ 0x240e, 0x01 },	/* EQ input mute control */
	{ 0x2301, CS42_HP_MIX_VOL },	/* mixer channel A input volume */
	{ 0x2303, CS42_HP_MIX_VOL },	/* mixer channel B input volume */
	{ 0x1101, 0x96 },	/* power up */
};

/* Same order as the SP-1-dev reference sequence. Jack detection is set up
 * by the last writes, so the jack is only read once all of them went through
 * (cs42_ok).
 */
static const struct reg16 cs42_hp_seq[] = {
	{ 0x1121, 0x41 },	/* headset switch control */
	{ 0x1b74, 0x03 },	/* misc detect control */
	{ 0x1129, 0x01 },	/* headset clamp disable */
	{ 0x2001, 0x0d },	/* HP control: muted until audio runs */
	/* DAC control 2: the reference 0x84 with the DAC high-pass (DC
	 * blocking, bit 1) left on as after reset
	 */
	{ 0x1f06, 0x86 },
	{ 0x2301, CS42_HP_MIX_VOL },	/* mixer channel A input volume */
	{ 0x2303, CS42_HP_MIX_VOL },	/* mixer channel B input volume */
	{ 0x1b73, 0xc2 },	/* tip sense control 2: jack detect */
	{ 0x1b75, 0x9f },	/* mic detect control 1 */
};

static int cs42_init_once(uint32_t reset_ms, uint32_t settle_ms)
{
	uint8_t osc = 0;

	/* 1: reset asserted before SCLK and LRCK (on the retry too), 2: release,
	 * wait 2.5 ms
	 */
	board_osc_enable(false);
	board_cs42_reset(true);
	k_msleep(reset_ms);
	board_osc_enable(true);
	board_cs42_reset(false);
	k_msleep(settle_ms);

	if (cs42_write_seq(cs42_clock_seq, ARRAY_SIZE(cs42_clock_seq))) {
		st.cs42_step = 1;
		return -EIO;
	}

	/* Wait for 0x1109 = 0x02: SCLK selected as internal MCLK (bits 1:0)
	 * and the RC oscillator powered down (bit 2). Like the reference
	 * code, go on after the timeout: the status is kept for the console.
	 */
	for (int i = 0; i < 20; i++) {
		k_msleep(1);
		if (cs42_read(0x1109, &osc) == 0 && (osc & 0x07) == 0x02) {
			break;
		}
	}
	st.osc_stat = osc;

	if (cs42_write_seq(cs42_port_seq, ARRAY_SIZE(cs42_port_seq))) {
		st.cs42_step = 2;
		return -EIO;
	}

	/* Headphone amplifier is operational 10 ms after power up */
	k_msleep(10);
	safety_wdt_feed();

	if (cs42_write_seq(cs42_hp_seq, ARRAY_SIZE(cs42_hp_seq))) {
		st.cs42_step = 3;
		return -EIO;
	}
	st.cs42_step = 0;

	return 0;
}

static int cs42_init(void)
{
	st.cs42_tries = 1;
	if (cs42_init_once(1, 3) == 0) {
		return 0;
	}

	/* Once more with the longer reset of the reference firmware */
	safety_wdt_feed();
	st.cs42_tries = 2;

	return cs42_init_once(5, 10);
}

struct reg8 {
	uint8_t page;
	uint8_t reg;
	uint8_t val;
	uint8_t delay_ms;
};

static const struct reg8 tas_seq[] = {
	{ 0, 0x01, 0x01, 5 },	/* software reset */
	{ 1, 0x02, 0x00, 0 },	/* LDO 1.8 V, level shifters on */
	/* PLL from BCLK: 3.072 MHz x 32 = 98.304 MHz,
	 * NDAC 2 x MDAC 8 x DOSR 128 = 2048 -> 48 kHz
	 */
	{ 0, 0x04, 0x07, 0 },	/* PLL_CLKIN = BCLK, CODEC_CLKIN = PLL */
	{ 0, 0x05, 0x91, 0 },	/* PLL on, P = 1, R = 1 */
	{ 0, 0x06, 0x20, 0 },	/* J = 32 */
	{ 0, 0x07, 0x00, 0 },	/* D = 0 */
	{ 0, 0x08, 0x00, 15 },	/* PLL lock time */
	{ 0, 0x0b, 0x82, 0 },	/* NDAC = 2, on */
	{ 0, 0x0c, 0x88, 0 },	/* MDAC = 8, on */
	{ 0, 0x0d, 0x00, 0 },	/* DOSR = 128 */
	{ 0, 0x0e, 0x80, 0 },
	{ 0, 0x1b, 0x20, 0 },	/* I2S, 24 bit, BCLK/WCLK inputs */
	{ 0, 0x1c, 0x00, 0 },	/* data offset 0 */
	{ 0, 0x3c, 0x02, 0 },	/* processing block PRB_P2 */
	{ 0, 0x3f, TAS_DAC_PATH_MIX, 0 },
	{ 0, 0x41, 0x00, 0 },	/* DAC digital gain 0 dB */
	{ 0, 0x40, 0x04, 0 },	/* DAC unmuted */
	{ 1, 0x01, 0x10, 0 },	/* master reference on */
	{ 1, 0x0a, 0x00, 0 },	/* common mode 0.9 V */
	{ 1, 0x0c, 0x04, 0 },	/* DAC routed to the output mixer */
	{ 1, 0x16, 0x00, 0 },
	{ 1, 0x18, 0x00, 0 },
	{ 1, 0x2e, 0x00, 0 },	/* speaker attenuation 0 dB */
	{ 1, 0x30, TAS_SPK_GAIN_MUTE, 0 },
	{ 1, 0x2d, 0x02, 0 },	/* speaker driver on */
	{ 0, 0x00, 0x00, 0 },	/* back to page 0 */
};

static int tas_init(void)
{
	board_tas_reset(true);
	k_msleep(1);
	board_tas_reset(false);
	/* No register writes within 1 ms of reset release */
	k_msleep(5);

	for (size_t i = 0; i < ARRAY_SIZE(tas_seq); i++) {
		if (tas_write(tas_seq[i].page, tas_seq[i].reg, tas_seq[i].val) != 0) {
			st.tas_step = (uint8_t)(i + 1);
			return -EIO;
		}
		if (tas_seq[i].delay_ms) {
			k_msleep(tas_seq[i].delay_ms);
		}
	}

	return 0;
}

int codec_init(void)
{
	if (!device_is_ready(i2c)) {
		return -ENODEV;
	}

	st.cs42_ok = cs42_init() == 0;
	safety_wdt_feed();
	/* The TAS2505 PLL locks to the bit clock, which runs from here on. */
	st.tas_ok = tas_init() == 0;
	st.speaker_on = false;

	if (!st.cs42_ok) {
		return -EIO;
	}

	return st.tas_ok ? 0 : -EIO;
}

void codec_power_down(void)
{
	/* The reference firmware's pop-free order: mute, let it settle, then
	 * reset both chips and stop the clock
	 */
	if (st.tas_ok) {
		(void)tas_write(1, 0x30, TAS_SPK_GAIN_MUTE);
	}
	if (st.cs42_answers) {
		(void)cs42_write(0x2001, 0x0d);
	}
	k_msleep(30);
	if (st.tas_ok) {
		(void)tas_write(0, 0x01, 0x01);		/* software reset */
	}
	board_tas_reset(true);
	board_cs42_reset(true);
	board_osc_enable(false);

	k_mutex_lock(&lock, K_FOREVER);
	st.cs42_ok = false;
	st.cs42_answers = false;
	st.tas_ok = false;
	st.speaker_on = false;
	st.jack_valid = false;
	k_mutex_unlock(&lock);
}

void codec_headphones_mute(bool mute)
{
	/* Muting needs only a chip that answers; playing needs the whole
	 * set-up, jack detection included
	 */
	if (mute ? st.cs42_answers : st.cs42_ok) {
		(void)cs42_write(0x2001, mute ? 0x0d : 0x01);
	}
}

bool codec_speaker_enable(bool on)
{
	if (on == st.speaker_on) {
		return true;
	}
	if (!st.tas_ok) {
		/* Nothing to switch: off is what it is */
		return !on;
	}
	if (tas_write(1, 0x30, on ? TAS_SPK_GAIN_ON : TAS_SPK_GAIN_MUTE) == 0) {
		st.speaker_on = on;
	}

	return st.speaker_on == on;
}

int codec_headphones_read(bool *present)
{
	uint8_t jack;
	int err;

	/* Without the tip sense set-up the bit means nothing */
	if (!st.cs42_ok) {
		return -ENODEV;
	}
	err = cs42_read(0x1b77, &jack);
	if (err) {
		return err;
	}
	k_mutex_lock(&lock, K_FOREVER);
	st.jack = jack;
	st.jack_valid = true;
	k_mutex_unlock(&lock);
	*present = (jack & 0x80) != 0;

	return 0;
}

bool codec_jack_usable(void)
{
	return st.cs42_ok;
}

void codec_get_status(struct codec_status *out)
{
	k_mutex_lock(&lock, K_FOREVER);
	*out = st;
	k_mutex_unlock(&lock);
}

int codec_cs42_read(uint16_t reg, uint8_t *val)
{
	return cs42_read(reg, val);
}

int codec_tas_read(uint8_t page, uint8_t reg, uint8_t *val)
{
	return tas_read(page, reg, val);
}
