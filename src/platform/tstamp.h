/*
 * SPDX-License-Identifier: MIT
 *
 * Hardware timestamps for the clock estimator: TIMER2 runs at 16 MHz and PPI
 * captures it on every USB SOF (CC0) and every I2S TXPTRUPD, i.e. I2S block
 * boundary (CC1). No interrupts involved.
 */

#ifndef SP1M_TSTAMP_H_
#define SP1M_TSTAMP_H_

#include <stdint.h>

#define TSTAMP_HZ 16000000u

int tstamp_init(void);

/* Latest SOF capture and the USB frame number that belongs to it. */
void tstamp_sof(uint32_t *t, uint16_t *frame_no);

/* Latest I2S block boundary capture. */
uint32_t tstamp_block(void);

/* The timer now, TSTAMP_HZ: wall time that keeps running while the CPU
 * sleeps (unlike the DWT cycle counter). Audio thread only.
 */
uint32_t tstamp_now(void);

/* Current USB frame number (11 bit). */
uint16_t tstamp_frame_no(void);

#endif /* SP1M_TSTAMP_H_ */
