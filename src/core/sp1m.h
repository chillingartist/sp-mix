/*
 * SPDX-License-Identifier: MIT
 *
 * Shared constants of the SP-1 Mixer firmware. Everything under src/core is
 * plain C without Zephyr dependencies so it can be unit tested on a host.
 */

#ifndef SP1M_H_
#define SP1M_H_

#include <stdbool.h>
#include <stdint.h>

#define SP1M_FS			48000
#define SP1M_TRACKS		4
/* Two USB channels per track: Track N uses channels 2N and 2N+1 */
#define SP1M_IN_CH		(2 * SP1M_TRACKS)
/* Frames per I2S block */
#define SP1M_BLOCK_FRAMES	64
/* Nominal frames per 1 ms USB frame */
#define SP1M_USB_FRAMES_NOM	(SP1M_FS / 1000)
/* Adaptive endpoints may carry one extra frame */
#define SP1M_USB_FRAMES_MAX	(SP1M_USB_FRAMES_NOM + 1)
#define SP1M_USB_FRAME_BYTES	(SP1M_IN_CH * 2)

#define SP1M_ARRAY_SIZE(a)	(sizeof(a) / sizeof((a)[0]))

static inline float sp1m_clampf(float x, float lo, float hi)
{
	return x < lo ? lo : (x > hi ? hi : x);
}

static inline int32_t sp1m_clampi(int32_t x, int32_t lo, int32_t hi)
{
	return x < lo ? lo : (x > hi ? hi : x);
}

#endif /* SP1M_H_ */
