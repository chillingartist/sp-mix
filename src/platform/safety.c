/*
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

#include <zephyr/arch/cpu.h>
#include <zephyr/fatal.h>
#include <zephyr/irq.h>
#include <zephyr/kernel.h>
#include <zephyr/linker/section_tags.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/reboot.h>
#include <nrf.h>

#include "board_io.h"
#include "safety.h"

#define FAULT_MAGIC		0xfa17fa17u
/* Upper 24 bits of the RAM copy of the culprit code */
#define CULPRIT_MAGIC		0xc0de5100u
/* Magic the bootloader may honour to stay in update mode */
#define GPREGRET_DFU		0x57
/* A stalled heartbeat stops the feeding after this long */
#define HB_TIMEOUT_MS		500
#define SUPERVISE_PERIOD_MS	100
/* Our own watchdog setup when the bootloader did not start it */
#define WDT_TIMEOUT_S		8

struct fault_rec {
	uint32_t magic;
	uint32_t reason;
	uint32_t pc;
};

/* Survives soft and watchdog resets (measured with `selftest`) */
static __noinit struct fault_rec fault_rec;
static __noinit volatile uint32_t culprit_ram;

static uint32_t reset_reason;
static struct safety_fault last_fault;

static atomic_t hb_count[SAFETY_HB_COUNT];
static atomic_t hb_expected;
static atomic_val_t hb_seen[SAFETY_HB_COUNT];
static uint32_t hb_last_ms[SAFETY_HB_COUNT];
static uint32_t next_check_ms;
static bool starved;

static uint8_t thread_code(k_tid_t t)
{
	static const char *const names[] = {
		[SAFETY_THREAD_MAIN] = "main",
		[SAFETY_THREAD_IDLE] = "idle",
		[SAFETY_THREAD_SYSWORKQ] = "sysworkq",
		[SAFETY_THREAD_USBD] = "usbd",
		[SAFETY_THREAD_UDC] = "udc_nrfx",
		[SAFETY_THREAD_CONSOLE] = "console",
		[SAFETY_THREAD_STORE] = "store",
		[SAFETY_THREAD_AUDIO] = "audio",
	};
	const char *name = k_thread_name_get(t);

	for (size_t i = 1; name != NULL && i < ARRAY_SIZE(names); i++) {
		if (strcmp(name, names[i]) == 0) {
			return (uint8_t)i;
		}
	}

	return SAFETY_THREAD_OTHER;
}

/*
 * The watchdog fires TIMEOUT two 32 kHz cycles before it resets the chip.
 * Zero latency, so it also runs inside irq_lock() sections: note who held
 * the CPU.
 */
ISR_DIRECT_DECLARE(wdt_timeout_isr)
{
	uint8_t code = 0;

	for (uint32_t w = 0; w < 2 && code == 0; w++) {
		uint32_t active = NVIC->IABR[w];

		if (w == (uint32_t)WDT_IRQn / 32u) {
			active &= ~BIT((uint32_t)WDT_IRQn % 32u);
		}
		if (active) {
			code = SAFETY_CULPRIT_IRQ |
			       ((w * 32u + (uint32_t)__builtin_ctz(active)) & 0x3fu);
		}
	}
	if (code == 0) {
		code = SAFETY_CULPRIT_THREAD | thread_code(k_current_get());
	}
	if (NRF_POWER->GPREGRET2 == 0) {
		safety_set_culprit(code);
	}
	/* About 60 us left: silence the codecs before the pins float */
	board_codecs_kill();
	NRF_WDT->EVENTS_TIMEOUT = 0;

	/* No kernel calls from a zero latency ISR: never reschedule */
	return 0;
}

void safety_boot(void)
{
	reset_reason = NRF_POWER->RESETREAS;
	NRF_POWER->RESETREAS = 0xffffffff;
	/* A stale update-mode request must not linger past this boot. */
	NRF_POWER->GPREGRET = 0;

	uint8_t reg = (uint8_t)NRF_POWER->GPREGRET2;
	uint8_t ram = (culprit_ram & 0xffffff00u) == CULPRIT_MAGIC ? (uint8_t)culprit_ram : 0;

	last_fault.culprit = reg ? reg : ram;
	last_fault.kept = (reg ? SAFETY_KEPT_REG : 0) | (ram ? SAFETY_KEPT_RAM : 0);
	NRF_POWER->GPREGRET2 = 0;
	culprit_ram = 0;
	if (fault_rec.magic == FAULT_MAGIC) {
		last_fault.valid = true;
		last_fault.reason = fault_rec.reason;
		last_fault.pc = fault_rec.pc;
	}
	fault_rec.magic = 0;

	if (!(NRF_WDT->RUNSTATUS & WDT_RUNSTATUS_RUNSTATUS_Msk)) {
		/* Started without the bootloader (debugger): own setup.
		 * A running watchdog cannot be reconfigured, so the
		 * bootloader's settings are kept otherwise.
		 */
		NRF_WDT->CONFIG = (WDT_CONFIG_SLEEP_Run << WDT_CONFIG_SLEEP_Pos) |
				  (WDT_CONFIG_HALT_Pause << WDT_CONFIG_HALT_Pos);
		NRF_WDT->CRV = WDT_TIMEOUT_S * 32768u - 1u;
		NRF_WDT->RREN = WDT_RREN_RR0_Msk;
		NRF_WDT->TASKS_START = 1;
	}

	IRQ_DIRECT_CONNECT(WDT_IRQn, 0, wdt_timeout_isr, IRQ_ZERO_LATENCY);
	NRF_WDT->EVENTS_TIMEOUT = 0;
	NRF_WDT->INTENSET = WDT_INTENSET_TIMEOUT_Msk;
	irq_enable(WDT_IRQn);

	safety_wdt_feed();
}

uint32_t safety_reset_reason(void)
{
	return reset_reason;
}

void safety_last_fault(struct safety_fault *out)
{
	*out = last_fault;
}

void safety_wdt_feed(void)
{
	/* Reload every enabled request register; the bootloader decides
	 * which ones are enabled.
	 */
	uint32_t rren = NRF_WDT->RREN;

	for (int i = 0; i < 8; i++) {
		if (rren & BIT(i)) {
			NRF_WDT->RR[i] = WDT_RR_RR_Reload;
		}
	}
}

void safety_heartbeat(enum safety_hb hb)
{
	atomic_inc(&hb_count[hb]);
}

void safety_expect(enum safety_hb hb, bool expected)
{
	if (expected) {
		hb_last_ms[hb] = k_uptime_get_32();
		hb_seen[hb] = atomic_get(&hb_count[hb]);
		atomic_set_bit(&hb_expected, hb);
	} else {
		atomic_clear_bit(&hb_expected, hb);
	}
}

void safety_supervise(uint32_t now_ms)
{
	if ((int32_t)(now_ms - next_check_ms) < 0) {
		return;
	}
	next_check_ms = now_ms + SUPERVISE_PERIOD_MS;

	for (int i = 0; i < SAFETY_HB_COUNT; i++) {
		atomic_val_t c = atomic_get(&hb_count[i]);

		if (c != hb_seen[i]) {
			hb_seen[i] = c;
			hb_last_ms[i] = now_ms;
		} else if (atomic_test_bit(&hb_expected, i) &&
			   now_ms - hb_last_ms[i] > HB_TIMEOUT_MS && !starved) {
			/* Let the watchdog reset us, and say why. */
			starved = true;
			safety_set_culprit(SAFETY_CULPRIT_AUDIO);
		}
	}

	if (!starved) {
		safety_wdt_feed();
	}
}

void safety_set_culprit(uint8_t code)
{
	NRF_POWER->GPREGRET2 = code;
	culprit_ram = CULPRIT_MAGIC | code;
}

void safety_selftest_wdt(void)
{
	safety_set_culprit(SAFETY_CULPRIT_TEST);
	starved = true;
}

FUNC_NORETURN void safety_reset(uint8_t culprit)
{
	safety_set_culprit(culprit);
	board_codecs_kill();
	__DSB();
	NVIC_SystemReset();
	CODE_UNREACHABLE;
}

FUNC_NORETURN void safety_enter_bootloader(void)
{
	NRF_POWER->RESETREAS = 0xffffffff;
	NRF_POWER->GPREGRET = GPREGRET_DFU;
	board_codecs_kill();
	__DSB();
	NVIC_SystemReset();
	CODE_UNREACHABLE;
}

FUNC_NORETURN void safety_system_off(void)
{
	(void)irq_lock();
	NRF_POWER->RESETREAS = 0xffffffff;
	__DSB();
	NRF_POWER->SYSTEMOFF = 1;
	__DSB();
	for (;;) {
		/* Never reached: the chip is off. */
	}
}

void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *esf)
{
	fault_rec.reason = reason;
	fault_rec.pc = esf != NULL ? esf->basic.pc : 0;
	fault_rec.magic = FAULT_MAGIC;
	safety_set_culprit(SAFETY_CULPRIT_FATAL | (reason & 0x0f));
	board_codecs_kill();

	/* Reboot rather than hang: there is no reset button. */
	sys_reboot(SYS_REBOOT_COLD);
	CODE_UNREACHABLE;
}
