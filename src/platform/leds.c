/*
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <math.h>
#include <string.h>

#include <zephyr/drivers/pwm.h>

#include "leds.h"

static const struct pwm_dt_spec leds[LEDS_COUNT] = {
	PWM_DT_SPEC_GET(DT_NODELABEL(led_t1)),
	PWM_DT_SPEC_GET(DT_NODELABEL(led_t2)),
	PWM_DT_SPEC_GET(DT_NODELABEL(led_t3)),
	PWM_DT_SPEC_GET(DT_NODELABEL(led_t4)),
	PWM_DT_SPEC_GET(DT_NODELABEL(led_p1)),
	PWM_DT_SPEC_GET(DT_NODELABEL(led_p2)),
	PWM_DT_SPEC_GET(DT_NODELABEL(led_p3)),
	PWM_DT_SPEC_GET(DT_NODELABEL(led_p4)),
};

static uint16_t gamma_permille[256];
static uint8_t current[LEDS_COUNT];
static bool ready;

int leds_init(void)
{
	for (int i = 0; i < 256; i++) {
		gamma_permille[i] = (uint16_t)(powf((float)i / 255.0f, 2.2f) * 1000.0f + 0.5f);
	}
	/* A non-zero level of 1 should still glow */
	gamma_permille[1] = 1;

	for (int i = 0; i < LEDS_COUNT; i++) {
		if (!pwm_is_ready_dt(&leds[i])) {
			return -ENODEV;
		}
		pwm_set_pulse_dt(&leds[i], 0);
		current[i] = 0;
	}
	ready = true;

	return 0;
}

void leds_set(const uint8_t level[LEDS_COUNT])
{
	if (!ready) {
		return;
	}

	for (int i = 0; i < LEDS_COUNT; i++) {
		if (level[i] == current[i]) {
			continue;
		}
		current[i] = level[i];
		pwm_set_pulse_dt(&leds[i],
				 (uint32_t)((uint64_t)leds[i].period *
					    gamma_permille[level[i]] / 1000u));
	}
}

void leds_all(uint8_t level)
{
	uint8_t l[LEDS_COUNT];

	memset(l, level, sizeof(l));
	leds_set(l);
}
