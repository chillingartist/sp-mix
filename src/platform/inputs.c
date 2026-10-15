/*
 * SPDX-License-Identifier: MIT
 *
 * The SAADC is driven directly, polled, without its interrupt. Zephyr's
 * driver (through nrfx) hangs adc_read() forever when an interrupt lands
 * between TASKS_START and its state update: the STARTED interrupt then
 * finds the old state, never triggers SAMPLE and END never comes. With USB
 * start-of-frame interrupts every millisecond that happened every few
 * seconds. Here nothing waits for the hardware: every call collects the
 * previous scan and starts the next one, and a scan that does not finish is
 * counted, aborted and started over.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/devicetree.h>
#include <zephyr/dt-bindings/adc/adc.h>
#include <zephyr/dt-bindings/adc/nrf-saadc.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/barrier.h>
#include <nrf.h>

#include "board_io.h"
#include "inputs.h"

#define USER_NODE	DT_PATH(zephyr_user)
#define ADC_NODE	DT_IO_CHANNELS_CTLR_BY_IDX(USER_NODE, 0)

/* zephyr,user io-channels order (board DTS) */
enum {
	CH_LADDER1,
	CH_LADDER2,
	CH_FADER1,
	CH_FADER2,
	CH_FADER3,
	CH_FADER4,
	CH_BATTERY,
	CH_COUNT,
};

BUILD_ASSERT(DT_NODE_HAS_COMPAT(ADC_NODE, nordic_nrf_saadc), "SAADC expected");
BUILD_ASSERT(DT_PROP_LEN(USER_NODE, io_channels) == CH_COUNT, "unexpected io-channels");
/* A missing or duplicated channel would leave MAXCNT unreachable: END would
 * never come
 */
#define REG_BIT(child) | BIT(DT_REG_ADDR(child))
BUILD_ASSERT((0 DT_FOREACH_CHILD_STATUS_OKAY(ADC_NODE, REG_BIT)) == BIT_MASK(CH_COUNT) &&
	     DT_CHILD_NUM_STATUS_OKAY(ADC_NODE) == CH_COUNT,
	     "SAADC channels 0..6 must each exist once");

/* One scan returns the channels in channel number order: io-channels index
 * i must be SAADC channel i.
 */
#define CHECK_IO_CHANNEL(node, prop, idx)					\
	BUILD_ASSERT(DT_SAME_NODE(DT_IO_CHANNELS_CTLR_BY_IDX(node, idx), ADC_NODE) && \
		     DT_IO_CHANNELS_INPUT_BY_IDX(node, idx) == idx,		\
		     "io-channels index must equal the SAADC channel");
DT_FOREACH_PROP_ELEM(USER_NODE, io_channels, CHECK_IO_CHANNEL)

/* The register setup below hard-codes what the DTS channels declare */
#define CHECK_CHANNEL(child)							\
	BUILD_ASSERT(DT_REG_ADDR(child) < CH_COUNT &&				\
		     DT_ENUM_HAS_VALUE(child, zephyr_gain, adc_gain_1_6) &&	\
		     DT_ENUM_HAS_VALUE(child, zephyr_reference, adc_ref_internal) && \
		     DT_PROP(child, zephyr_acquisition_time) ==			\
			     ADC_ACQ_TIME(ADC_ACQ_TIME_MICROSECONDS, 20) &&	\
		     DT_PROP(child, zephyr_resolution) == 12 &&			\
		     DT_PROP(child, zephyr_input_positive) >= NRF_SAADC_AIN0 &&	\
		     DT_PROP(child, zephyr_input_positive) <= NRF_SAADC_AIN7 &&	\
		     !DT_NODE_HAS_PROP(child, zephyr_input_negative) &&		\
		     !DT_PROP_OR(child, zephyr_differential, 0) &&		\
		     DT_PROP_OR(child, zephyr_oversampling, 0) == 0,		\
		     "SAADC channel setup differs from inputs.c");
DT_FOREACH_CHILD_STATUS_OKAY(ADC_NODE, CHECK_CHANNEL)

#define PSELP_ENTRY(child) [DT_REG_ADDR(child)] = DT_PROP(child, zephyr_input_positive),

/* NRF_SAADC_AINx in the DTS is the PSELP register value */
static const uint8_t pselp[CH_COUNT] = {
	DT_FOREACH_CHILD_STATUS_OKAY(ADC_NODE, PSELP_ENTRY)
};

#define CH_CONFIG ((SAADC_CH_CONFIG_RESP_Bypass << SAADC_CH_CONFIG_RESP_Pos) |	\
		   (SAADC_CH_CONFIG_RESN_Bypass << SAADC_CH_CONFIG_RESN_Pos) |	\
		   (SAADC_CH_CONFIG_GAIN_Gain1_6 << SAADC_CH_CONFIG_GAIN_Pos) |	\
		   (SAADC_CH_CONFIG_REFSEL_Internal << SAADC_CH_CONFIG_REFSEL_Pos) | \
		   (SAADC_CH_CONFIG_TACQ_20us << SAADC_CH_CONFIG_TACQ_Pos) |	\
		   (SAADC_CH_CONFIG_MODE_SE << SAADC_CH_CONFIG_MODE_Pos) |	\
		   (SAADC_CH_CONFIG_BURST_Disabled << SAADC_CH_CONFIG_BURST_Pos))

/* STARTED follows START within a few cycles; this is far beyond that */
#define START_WAIT_US		50
#define STOP_WAIT_US		100
/* A scan takes 7 x (20 + 2) us; the control loop comes back after 2 ms */
#define SCAN_TIMEOUT_MS		10
/* Consecutive failed scans before the SAADC is power-cycled */
#define FAILS_BEFORE_POWER_CYCLE 3

/* EasyDMA target */
static int16_t buf[CH_COUNT] __aligned(4);
static bool busy;
static uint32_t started_ms;
static uint32_t fails;
static struct inputs_stats stats;

static void clear_events(void)
{
	NRF_SAADC->EVENTS_STARTED = 0;
	NRF_SAADC->EVENTS_END = 0;
	NRF_SAADC->EVENTS_DONE = 0;
	NRF_SAADC->EVENTS_RESULTDONE = 0;
	NRF_SAADC->EVENTS_CALIBRATEDONE = 0;
	NRF_SAADC->EVENTS_STOPPED = 0;
}

static bool wait_event(volatile uint32_t *event, uint32_t us)
{
	for (uint32_t i = 0; i <= us; i++) {
		if (*event) {
			return true;
		}
		k_busy_wait(1);
	}

	return false;
}

/* Abort whatever the SAADC is doing and leave it disabled. */
static void saadc_reset(void)
{
	if (NRF_SAADC->ENABLE) {
		NRF_SAADC->EVENTS_STOPPED = 0;
		NRF_SAADC->TASKS_STOP = 1;
		(void)wait_event(&NRF_SAADC->EVENTS_STOPPED, STOP_WAIT_US);
	}
	NRF_SAADC->ENABLE = SAADC_ENABLE_ENABLE_Disabled;
	clear_events();
	busy = false;
}

static void saadc_configure(void)
{
	/* Polled: keep the interrupt out of it entirely */
	NRF_SAADC->INTENCLR = 0xffffffff;
	NVIC_DisableIRQ(SAADC_IRQn);
	NVIC_ClearPendingIRQ(SAADC_IRQn);

	NRF_SAADC->RESOLUTION = SAADC_RESOLUTION_VAL_12bit;
	NRF_SAADC->OVERSAMPLE = SAADC_OVERSAMPLE_OVERSAMPLE_Bypass;
	NRF_SAADC->SAMPLERATE = SAADC_SAMPLERATE_MODE_Task << SAADC_SAMPLERATE_MODE_Pos;

	for (int i = 0; i < SAADC_CH_NUM; i++) {
		NRF_SAADC->CH[i].PSELN = SAADC_CH_PSELN_PSELN_NC;
		NRF_SAADC->CH[i].CONFIG = CH_CONFIG;
		NRF_SAADC->CH[i].PSELP = i < CH_COUNT ? pselp[i] : SAADC_CH_PSELP_PSELP_NC;
	}
}

/*
 * Last resort after repeated failures: power-cycle the peripheral the way
 * nrfx's anomaly 212 workaround does (it did so before every read), keeping
 * the three undocumented registers it preserves, then set it up again.
 */
static void saadc_power_cycle(void)
{
	volatile uint32_t *const power = (volatile uint32_t *)0x40007ffcu;
	volatile uint32_t *const r640 = (volatile uint32_t *)0x40007640u;
	volatile uint32_t *const r644 = (volatile uint32_t *)0x40007644u;
	volatile uint32_t *const r648 = (volatile uint32_t *)0x40007648u;
	uint32_t v640 = *r640;
	uint32_t v644 = *r644;
	uint32_t v648 = *r648;

	*power = 0;
	*power = 1;
	*r640 = v640;
	*r644 = v644;
	*r648 = v648;

	saadc_configure();
	stats.power_cycles++;
}

static void scan_failed(void)
{
	saadc_reset();
	if (++fails >= FAILS_BEFORE_POWER_CYCLE) {
		fails = 0;
		saadc_power_cycle();
	}
}

static bool scan_start(void)
{
	clear_events();
	NRF_SAADC->RESULT.PTR = (uint32_t)(uintptr_t)buf;
	NRF_SAADC->RESULT.MAXCNT = CH_COUNT;
	NRF_SAADC->ENABLE = SAADC_ENABLE_ENABLE_Enabled;

	/* Latch the buffer, then sample every enabled channel once */
	NRF_SAADC->TASKS_START = 1;
	if (!wait_event(&NRF_SAADC->EVENTS_STARTED, START_WAIT_US)) {
		stats.no_start++;
		scan_failed();
		return false;
	}
	NRF_SAADC->EVENTS_STARTED = 0;
	NRF_SAADC->TASKS_SAMPLE = 1;

	busy = true;
	started_ms = k_uptime_get_32();

	return true;
}

int inputs_init(void)
{
	board_btn_supply(true);
	saadc_reset();
	saadc_configure();

	return 0;
}

void inputs_shutdown(void)
{
	saadc_reset();
}

static int16_t clamp_raw(int16_t v)
{
	/* Single ended: small negative offsets read below zero */
	return v < 0 ? 0 : v;
}

int inputs_scan(struct raw_inputs *out)
{
	int ret = -EAGAIN;

	out->fn = board_fn_pressed();

	if (busy) {
		if (NRF_SAADC->EVENTS_END) {
			uint32_t amount = NRF_SAADC->RESULT.AMOUNT;

			/* END comes after the last result reached RAM */
			barrier_dmem_fence_full();
			NRF_SAADC->EVENTS_END = 0;
			NRF_SAADC->ENABLE = SAADC_ENABLE_ENABLE_Disabled;
			busy = false;

			if (amount == CH_COUNT) {
				fails = 0;
				out->ladder1 = clamp_raw(buf[CH_LADDER1]);
				out->ladder2 = clamp_raw(buf[CH_LADDER2]);
				out->fader[0] = clamp_raw(buf[CH_FADER1]);
				out->fader[1] = clamp_raw(buf[CH_FADER2]);
				out->fader[2] = clamp_raw(buf[CH_FADER3]);
				out->fader[3] = clamp_raw(buf[CH_FADER4]);
				out->battery = clamp_raw(buf[CH_BATTERY]);
				stats.scans++;
				ret = 0;
			} else {
				stats.short_scans++;
				scan_failed();
				ret = -EIO;
			}
		} else if (k_uptime_get_32() - started_ms > SCAN_TIMEOUT_MS &&
			   !NRF_SAADC->EVENTS_END) {
			/* END checked again: a preemption between the two
			 * reads must not discard a finished scan
			 */
			stats.timeouts++;
			scan_failed();
			ret = -ETIMEDOUT;
		} else {
			stats.not_ready++;
			return -EAGAIN;
		}
	}

	/* A scan that cannot even start is a failure, not "still running":
	 * the caller must see it to escalate
	 */
	if (!scan_start() && ret == -EAGAIN) {
		ret = -EIO;
	}

	return ret;
}

void inputs_get_stats(struct inputs_stats *out)
{
	/* One consistent set: the control loop cannot run in between */
	unsigned int key = irq_lock();

	*out = stats;
	irq_unlock(key);
}
