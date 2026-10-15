/*
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>

#include <hal/nrf_i2s.h>
#include <hal/nrf_timer.h>
#include <hal/nrf_usbd.h>
#include <helpers/nrfx_gppi.h>

#include "tstamp.h"

#define TIMER NRF_TIMER2

BUILD_ASSERT(TIMER2_CC_NUM > 2, "CC0 SOF, CC1 block, CC2 now");

int tstamp_init(void)
{
	uint8_t ch_sof, ch_blk;

	nrf_timer_task_trigger(TIMER, NRF_TIMER_TASK_STOP);
	/* The bootloader may have left the timer set up: no shortcuts (a
	 * COMPAREn_CLEAR would reset the count at random captures)
	 */
	nrf_timer_shorts_set(TIMER, 0);
	for (int i = 0; i < TIMER2_CC_NUM; i++) {
		nrf_timer_event_clear(TIMER, nrf_timer_compare_event_get(i));
	}
	nrf_timer_mode_set(TIMER, NRF_TIMER_MODE_TIMER);
	nrf_timer_bit_width_set(TIMER, NRF_TIMER_BIT_WIDTH_32);
	nrf_timer_prescaler_set(TIMER, 0);	/* 16 MHz */
	nrf_timer_int_disable(TIMER, 0xffffffff);
	nrf_timer_task_trigger(TIMER, NRF_TIMER_TASK_CLEAR);
	nrf_timer_task_trigger(TIMER, NRF_TIMER_TASK_START);

	if (nrfx_gppi_channel_alloc(&ch_sof) != NRFX_SUCCESS ||
	    nrfx_gppi_channel_alloc(&ch_blk) != NRFX_SUCCESS) {
		return -EBUSY;
	}

	nrfx_gppi_channel_endpoints_setup(
		ch_sof,
		nrf_usbd_event_address_get(NRF_USBD, NRF_USBD_EVENT_SOF),
		nrf_timer_task_address_get(TIMER, NRF_TIMER_TASK_CAPTURE0));
	nrfx_gppi_channel_endpoints_setup(
		ch_blk,
		nrf_i2s_event_address_get(NRF_I2S0, NRF_I2S_EVENT_TXPTRUPD),
		nrf_timer_task_address_get(TIMER, NRF_TIMER_TASK_CAPTURE1));
	/* No second task left over on these channels */
	nrfx_gppi_fork_endpoint_setup(ch_sof, 0);
	nrfx_gppi_fork_endpoint_setup(ch_blk, 0);
	nrfx_gppi_channels_enable(BIT(ch_sof) | BIT(ch_blk));

	return 0;
}

uint16_t tstamp_frame_no(void)
{
	return (uint16_t)(nrf_usbd_framecntr_get(NRF_USBD) & 0x7ff);
}

void tstamp_sof(uint32_t *t, uint16_t *frame_no)
{
	uint16_t f1, f2;
	uint32_t c;

	/* The frame counter and the capture change together at SOF;
	 * retry if one happened in between.
	 */
	do {
		f1 = tstamp_frame_no();
		c = nrf_timer_cc_get(TIMER, NRF_TIMER_CC_CHANNEL0);
		f2 = tstamp_frame_no();
	} while (f1 != f2);

	*t = c;
	*frame_no = f1;
}

uint32_t tstamp_block(void)
{
	return nrf_timer_cc_get(TIMER, NRF_TIMER_CC_CHANNEL1);
}

uint32_t tstamp_now(void)
{
	/* CC2 is free; only the audio thread captures into it */
	nrf_timer_task_trigger(TIMER, NRF_TIMER_TASK_CAPTURE2);

	return nrf_timer_cc_get(TIMER, NRF_TIMER_CC_CHANNEL2);
}
