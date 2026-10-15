/*
 * SPDX-License-Identifier: MIT
 *
 * Raw control inputs: one SAADC scan of both button ladders, the four faders
 * and the battery divider, plus the FUNCTION button.
 */

#ifndef SP1M_INPUTS_H_
#define SP1M_INPUTS_H_

#include <stdbool.h>
#include <stdint.h>

#include "sp1m.h"

struct raw_inputs {
	int16_t ladder1;
	int16_t ladder2;
	int16_t fader[SP1M_TRACKS];
	int16_t battery;
	bool fn;
};

struct inputs_stats {
	uint32_t scans;		/* completed scans */
	uint32_t no_start;	/* START did not latch the buffer */
	uint32_t timeouts;	/* the scan never ended */
	uint32_t short_scans;	/* ended with fewer results than channels */
	uint32_t not_ready;	/* called while a scan was still running */
	uint32_t power_cycles;	/* recoveries after repeated failures */
};

int inputs_init(void);

/*
 * Never blocks: collects the scan started by the previous call and starts
 * the next one, so the values are one call old (a scan takes about 154 us).
 * Returns 0 with fresh values, -EAGAIN while the scan is still running
 * (nothing changed), or another negative error after a failed scan: the
 * SAADC has been reset (power-cycled after repeated failures) and the next
 * call starts over. out->fn is always fresh.
 */
int inputs_scan(struct raw_inputs *out);

/* Stops the SAADC; the next inputs_scan() starts it again. */
void inputs_shutdown(void);

void inputs_get_stats(struct inputs_stats *out);

#endif /* SP1M_INPUTS_H_ */
