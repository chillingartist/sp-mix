/*
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/init.h>
#include <hal/nrf_gpio.h>
#include <nrf.h>
#include <soc/nrfx_coredep.h>
#include <soc_nrf_common.h>

#include "board_io.h"

#define USER DT_PATH(zephyr_user)

static const struct gpio_dt_spec fn = GPIO_DT_SPEC_GET(USER, fn_gpios);
static const struct gpio_dt_spec btn_com = GPIO_DT_SPEC_GET(USER, btn_com_gpios);
static const struct gpio_dt_spec osc_en = GPIO_DT_SPEC_GET(USER, osc_en_gpios);
static const struct gpio_dt_spec cs42_rst = GPIO_DT_SPEC_GET(USER, cs42_rst_gpios);
static const struct gpio_dt_spec tas_rst = GPIO_DT_SPEC_GET(USER, tas_rst_gpios);
static const struct gpio_dt_spec chg_en = GPIO_DT_SPEC_GET(USER, chg_en_gpios);
static const struct gpio_dt_spec chg_stat = GPIO_DT_SPEC_GET(USER, chg_stat_gpios);
static const struct gpio_dt_spec emmc_vccq = GPIO_DT_SPEC_GET(USER, emmc_vccq_gpios);
static const struct gpio_dt_spec emmc_rst = GPIO_DT_SPEC_GET(USER, emmc_rst_gpios);
static const struct gpio_dt_spec bt_rst = GPIO_DT_SPEC_GET(USER, bt_rst_gpios);

/* LED pins, also parked low before SYSTEM_OFF (they are PWM outputs). */
static const uint32_t led_pins[] = {
	NRF_GPIO_PIN_MAP(0, 29), NRF_GPIO_PIN_MAP(0, 26),
	NRF_GPIO_PIN_MAP(1, 15), NRF_GPIO_PIN_MAP(1, 14),
	NRF_GPIO_PIN_MAP(1, 13), NRF_GPIO_PIN_MAP(0, 0),
	NRF_GPIO_PIN_MAP(1, 12), NRF_GPIO_PIN_MAP(0, 1),
};

/* eMMC bus pins: released (default configuration) while unused */
static const uint32_t emmc_bus_pins[] = {
	NRF_GPIO_PIN_MAP(0, 6), NRF_GPIO_PIN_MAP(0, 7), NRF_GPIO_PIN_MAP(0, 8),
};

/*
 * The bootloader jumps to the application without a reset and leaves some
 * peripherals running (PWM2, PWM3, SAADC and possibly their interrupts).
 * Quiesce them before any Zephyr driver initialises.
 */
static int sp1_quiesce_bootloader(void)
{
	NRF_PWM_Type *const pwm[] = { NRF_PWM0, NRF_PWM1, NRF_PWM2, NRF_PWM3 };

	for (size_t i = 0; i < ARRAY_SIZE(NVIC->ICER); i++) {
		NVIC->ICER[i] = 0xffffffff;
		NVIC->ICPR[i] = 0xffffffff;
	}

	for (size_t i = 0; i < ARRAY_SIZE(pwm); i++) {
		pwm[i]->INTENCLR = 0xffffffff;
		pwm[i]->TASKS_STOP = 1;
		pwm[i]->ENABLE = 0;
		for (int c = 0; c < 4; c++) {
			pwm[i]->PSEL.OUT[c] = PWM_PSEL_OUT_CONNECT_Disconnected
					      << PWM_PSEL_OUT_CONNECT_Pos;
		}
	}

	NRF_SAADC->INTENCLR = 0xffffffff;
	if (NRF_SAADC->ENABLE) {
		NRF_SAADC->EVENTS_STOPPED = 0;
		NRF_SAADC->TASKS_STOP = 1;
		for (int n = 0; n < 10000 && !NRF_SAADC->EVENTS_STOPPED; n++) {
		}
		NRF_SAADC->ENABLE = 0;
	}
	NRF_SAADC->EVENTS_STARTED = 0;
	NRF_SAADC->EVENTS_END = 0;
	NRF_SAADC->EVENTS_DONE = 0;
	NRF_SAADC->EVENTS_RESULTDONE = 0;
	NRF_SAADC->EVENTS_STOPPED = 0;

	return 0;
}

SYS_INIT(sp1_quiesce_bootloader, PRE_KERNEL_1, 0);

int board_io_init(void)
{
	int err = 0;

	/* Audio chips off until the audio path brings them up */
	err |= gpio_pin_configure_dt(&osc_en, GPIO_OUTPUT_INACTIVE);
	err |= gpio_pin_configure_dt(&cs42_rst, GPIO_OUTPUT_ACTIVE);
	err |= gpio_pin_configure_dt(&tas_rst, GPIO_OUTPUT_ACTIVE);

	/* Unused in v1 */
	err |= gpio_pin_configure_dt(&bt_rst, GPIO_OUTPUT_ACTIVE);
	err |= gpio_pin_configure_dt(&emmc_rst, GPIO_OUTPUT_ACTIVE);
	err |= gpio_pin_configure_dt(&emmc_vccq, GPIO_OUTPUT_INACTIVE);
	for (size_t i = 0; i < ARRAY_SIZE(emmc_bus_pins); i++) {
		nrf_gpio_cfg_default(emmc_bus_pins[i]);
	}

	/* Charging stays enabled: without /CE low the battery never charges. */
	err |= gpio_pin_configure_dt(&chg_en, GPIO_OUTPUT_ACTIVE);
	err |= gpio_pin_configure_dt(&chg_stat, GPIO_INPUT);

	err |= gpio_pin_configure_dt(&btn_com, GPIO_OUTPUT_ACTIVE);
	err |= gpio_pin_configure_dt(&fn, GPIO_INPUT);

	return err ? -EIO : 0;
}

void board_osc_enable(bool on)
{
	gpio_pin_set_dt(&osc_en, on);
}

void board_cs42_reset(bool assert)
{
	gpio_pin_set_dt(&cs42_rst, assert);
}

void board_tas_reset(bool assert)
{
	gpio_pin_set_dt(&tas_rst, assert);
}

/* Both codec resets are active low and the oscillator enable active high:
 * all three pins low means codecs in reset, oscillator off.
 */
BUILD_ASSERT((DT_GPIO_FLAGS(USER, cs42_rst_gpios) & GPIO_ACTIVE_LOW) &&
	     (DT_GPIO_FLAGS(USER, tas_rst_gpios) & GPIO_ACTIVE_LOW) &&
	     !(DT_GPIO_FLAGS(USER, osc_en_gpios) & GPIO_ACTIVE_LOW),
	     "board_codecs_kill() drives these pins low");

void board_codecs_kill(void)
{
	static const uint32_t pins[] = {
		NRF_DT_GPIOS_TO_PSEL(USER, cs42_rst_gpios),
		NRF_DT_GPIOS_TO_PSEL(USER, tas_rst_gpios),
		NRF_DT_GPIOS_TO_PSEL(USER, osc_en_gpios),
	};

	for (size_t i = 0; i < ARRAY_SIZE(pins); i++) {
		nrf_gpio_pin_clear(pins[i]);
		nrf_gpio_cfg_output(pins[i]);
	}
	/* The callers reset the chip next: hold long enough for a valid
	 * reset pulse (the CS42L42 driver in Linux uses at least 10 us).
	 * A plain cycle loop, fine in the zero-latency watchdog ISR.
	 */
	nrfx_coredep_delay_us(50);
}

void board_btn_supply(bool on)
{
	gpio_pin_set_dt(&btn_com, on);
}

void board_charger_enable(bool on)
{
	gpio_pin_set_dt(&chg_en, on);
}

bool board_fn_pressed(void)
{
	return gpio_pin_get_dt(&fn) > 0;
}

bool board_charging(void)
{
	return gpio_pin_get_dt(&chg_stat) > 0;
}

bool board_usb_power(void)
{
	return (NRF_POWER->USBREGSTATUS & POWER_USBREGSTATUS_VBUSDETECT_Msk) != 0;
}

void board_prepare_system_off(void)
{
	/* Release the LED pins from PWM so their GPIO level applies. */
	NRF_PWM0->ENABLE = 0;
	NRF_PWM1->ENABLE = 0;

	/* Pin levels are retained in SYSTEM_OFF: drive everything off. */
	for (size_t i = 0; i < ARRAY_SIZE(led_pins); i++) {
		nrf_gpio_pin_clear(led_pins[i]);
		nrf_gpio_cfg_output(led_pins[i]);
	}

	gpio_pin_set_dt(&btn_com, 0);
	gpio_pin_set_dt(&tas_rst, 1);
	gpio_pin_set_dt(&cs42_rst, 1);
	gpio_pin_set_dt(&osc_en, 0);
	gpio_pin_set_dt(&emmc_vccq, 0);
	gpio_pin_set_dt(&bt_rst, 1);

	/* FUNCTION (P0.27, low when pressed) wakes the chip up. */
	nrf_gpio_cfg_sense_input(NRF_GPIO_PIN_MAP(0, 27), NRF_GPIO_PIN_PULLUP,
				 NRF_GPIO_PIN_SENSE_LOW);
}
