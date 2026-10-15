/*
 * SPDX-License-Identifier: MIT
 *
 * Control loop stall catcher. The control loop marks which stage it is in
 * and counts its iterations; two independent watchers (the USB SOF callback
 * in the usbd thread and a kernel timer) notice when the count stops. The
 * first one to notice records where the loop is stuck and what the main
 * thread waits on, then soft-resets: unlike a watchdog reset, a soft reset
 * keeps the record in RAM for the next boot.
 */

#ifndef SP1M_STALL_H_
#define SP1M_STALL_H_

#include <stdbool.h>
#include <stdint.h>

/* Where the control loop is; also the low nibble of the culprit code */
enum stall_stage {
	STALL_STAGE_NONE,
	STALL_STAGE_WAIT,	/* waiting for the next 2 ms tick */
	STALL_STAGE_INPUTS,	/* ADC scan */
	STALL_STAGE_OUTPUTS,	/* headphone poll, audio levels */
	STALL_STAGE_UI,
	STALL_STAGE_MIX,	/* mix parameters, speaker switch */
	STALL_STAGE_LEDS,
	STALL_STAGE_CMDS,	/* console commands */
	STALL_STAGE_PERSIST,
	STALL_STAGE_STATUS,	/* status snapshot, watchdog supervision */
	STALL_STAGE_SETTLE,	/* fader settle before the loop starts */
	STALL_STAGE_FLUSH,	/* synchronous scene save */
	STALL_STAGE_STANDBY,
	STALL_STAGE_OFF,
	STALL_STAGE_COUNT,
};

/*
 * GPREGRET2 culprit codes: | stage. "Timer dead" means the kernel timer
 * did not fire while the loop was stuck.
 */
#define STALL_CULPRIT		0x60
#define STALL_CULPRIT_NO_TIMER	0x70

#define STALL_BT_DEPTH		8

enum stall_src {
	STALL_SRC_SOF = 1,
	STALL_SRC_TIMER = 2,
};

struct stall_rec {
	uint32_t magic;
	uint32_t up_ms;
	uint32_t loops;
	uint8_t src;		/* enum stall_src */
	uint8_t stage;		/* enum stall_stage */
	uint8_t state;		/* main thread state bits */
	uint8_t current;	/* the main thread was running (1) */
	uint32_t wait;		/* wait queue the main thread is pended on */
	int32_t timeout;	/* its remaining timeout in ms, -1 none */
	uint32_t timer_beats;	/* kernel timer beats while stuck, ~0 unknown */
	uint32_t pc;		/* stacked PC and LR of the main thread */
	uint32_t lr;
	uint32_t bt[STALL_BT_DEPTH]; /* code addresses found on its stack */
	uint32_t saadc;		/* ENABLE, STATUS and events, see stall.c */
	uint32_t saadc_inten;
	uint32_t iser[2];
	uint32_t ispr[2];
	uint32_t rtc_counter;
	uint32_t rtc_cc0;
	uint32_t rtc_inten;
	uint32_t ppi_chen;
};

extern volatile uint8_t stall_cur_stage;
extern volatile uint32_t stall_loops;

/* Right after safety_boot(): latches the previous run's record. */
void stall_boot(void);

/* The previous run's record, if it ended in a stall reset. */
bool stall_last(struct stall_rec *out);

/* From the main thread once the control loop starts: arms the watchers. */
void stall_start(void);

/* Enter a stage; also accounts the time spent in the previous one. */
void stall_stage(enum stall_stage s);

/* The control loop made progress. */
static inline void stall_tick(void)
{
	stall_loops = stall_loops + 1u;
}

/* Every USB start of frame (1 ms), from the usbd thread. */
void stall_sof(void);

/* Kernel timer beats since boot, for the console. */
uint32_t stall_timer_beats(void);

/*
 * CPU cycles spent in each stage since boot (wall time, so it includes
 * preemption), and the longest single stay since the last call that asked
 * for a reset.
 */
struct stall_timing {
	uint64_t cycles[STALL_STAGE_COUNT];
	uint32_t max[STALL_STAGE_COUNT];
};

void stall_get_timing(struct stall_timing *out, bool reset_max);

#endif /* SP1M_STALL_H_ */
