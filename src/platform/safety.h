/*
 * SPDX-License-Identifier: MIT
 *
 * Watchdog supervision, crash breadcrumbs and the paths back to the
 * bootloader. The SP-1 has no reset button: whatever happens, the watchdog
 * and the Track 1 + Track 4 combination must keep working.
 */

#ifndef SP1M_SAFETY_H_
#define SP1M_SAFETY_H_

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/toolchain.h>

enum safety_hb {
	SAFETY_HB_AUDIO,
	SAFETY_HB_COUNT,
};

struct safety_fault {
	bool valid;		/* the previous run ended in a fatal error */
	uint32_t reason;
	uint32_t pc;
	uint8_t culprit;	/* culprit code of the previous run, 0 = none */
	uint8_t kept;		/* where it survived: SAFETY_KEPT_* bits */
};

#define SAFETY_KEPT_REG		BIT(0)	/* GPREGRET2 */
#define SAFETY_KEPT_RAM		BIT(1)	/* __noinit copy */

/*
 * Culprit codes, kept in GPREGRET2 and in __noinit RAM. Measured on an SP-1
 * (`selftest`): a soft reset keeps both, a watchdog reset clears GPREGRET2
 * but keeps RAM.
 */
#define SAFETY_CULPRIT_AUDIO	0x41	/* 'A': audio thread stalled */
#define SAFETY_CULPRIT_INPUTS	0x49	/* 'I': no SAADC scan for a second */
#define SAFETY_CULPRIT_TEST	0x4a	/* `selftest wdt`: feeding stopped on purpose */
/* Watchdog timeout while a thread ran: | enum safety_thread */
#define SAFETY_CULPRIT_THREAD	0x50
/* Watchdog timeout inside an interrupt: | IRQ number (0..63) */
#define SAFETY_CULPRIT_IRQ	0x80
#define SAFETY_CULPRIT_FATAL	0xf0	/* | reason & 0x0f */

enum safety_thread {
	SAFETY_THREAD_OTHER,
	SAFETY_THREAD_MAIN,
	SAFETY_THREAD_IDLE,
	SAFETY_THREAD_SYSWORKQ,
	SAFETY_THREAD_USBD,
	SAFETY_THREAD_UDC,
	SAFETY_THREAD_CONSOLE,
	SAFETY_THREAD_STORE,
	SAFETY_THREAD_AUDIO,
};

/*
 * First call in main(): latches and clears RESETREAS, GPREGRET and the
 * breadcrumbs, and makes sure the watchdog runs.
 */
void safety_boot(void);

uint32_t safety_reset_reason(void);
void safety_last_fault(struct safety_fault *out);

void safety_wdt_feed(void);

/* Leaves a culprit code for the next boot (callable from any context). */
void safety_set_culprit(uint8_t code);

/* Test: stop feeding the watchdog while the control loop keeps running. */
void safety_selftest_wdt(void);

void safety_heartbeat(enum safety_hb hb);

/* Enables or disables supervision of one heartbeat source. */
void safety_expect(enum safety_hb hb, bool expected);

/*
 * Called by the control loop on every tick. Feeds the watchdog only while
 * every expected heartbeat source keeps beating.
 */
void safety_supervise(uint32_t now_ms);

/* Soft reset back into the firmware, leaving a culprit code behind. */
FUNC_NORETURN void safety_reset(uint8_t culprit);

/* Soft reset into the bootloader; the user keeps holding Track 1 + 4. */
FUNC_NORETURN void safety_enter_bootloader(void);

/* SYSTEM_OFF; board_prepare_system_off() must have run. */
FUNC_NORETURN void safety_system_off(void);

#endif /* SP1M_SAFETY_H_ */
