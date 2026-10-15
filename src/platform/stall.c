/*
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/linker/linker-defs.h>
#include <zephyr/linker/section_tags.h>
#include <zephyr/sys/atomic.h>
#include <nrf.h>

#include "board_io.h"
#include "safety.h"
#include "stall.h"

#define STALL_MAGIC		0x57a11ed0u
/* No loop iteration for this long is a stall (normal gaps are < 400 ms) */
#define STALL_MS		1500u
#define TIMER_PERIOD_MS		50u

volatile uint8_t stall_cur_stage;
volatile uint32_t stall_loops;

/* Stage timing (main thread writes, console reads) */
static uint32_t stage_t0;
static uint64_t stage_cycles[STALL_STAGE_COUNT];
static uint32_t stage_max[STALL_STAGE_COUNT];
static volatile bool max_reset;

/* Survives the soft reset that follows a stall */
static __noinit struct stall_rec rec;
static struct stall_rec last;
static bool have_last;

static k_tid_t main_tid;
static atomic_t armed;
static atomic_t tripped;
static struct k_timer beat_timer;
static volatile uint32_t beats;

/* SOF watcher state (usbd thread only) */
static uint32_t sof_seen_loops;
static uint32_t sof_idle_ms;
static uint32_t sof_beats_at_progress;

/* Timer watcher state (timer ISR only) */
static uint32_t tmr_seen_loops;
static uint32_t tmr_idle;

static bool is_code(uint32_t w)
{
	return (w & 1u) && w >= (uint32_t)(uintptr_t)__text_region_start &&
	       w < (uint32_t)(uintptr_t)__text_region_end;
}

static void snapshot_thread(void)
{
	const struct k_thread *t = main_tid;
	uint32_t psp, top;
	size_t n = 0;

	rec.state = t->base.thread_state;
	rec.wait = (uint32_t)(uintptr_t)t->base.pended_on;
	/* Remaining wait in ms, -1 for none or forever */
	rec.timeout = sys_dnode_is_linked(&t->base.timeout.node) ?
		      (int32_t)k_ticks_to_ms_floor64(z_timeout_remaining(&t->base.timeout)) :
		      -1;

	/* Interrupted while running: its frame is on the live PSP */
	rec.current = k_current_get() == main_tid;
	psp = rec.current ? __get_PSP() : t->callee_saved.psp;
	top = (uint32_t)(t->stack_info.start + t->stack_info.size);
	if (psp < t->stack_info.start || psp + 32u > top || (psp & 3u)) {
		return;
	}

	/* Exception frame: r0-r3, r12, lr, pc, xpsr */
	const uint32_t *frame = (const uint32_t *)psp;
	bool fp_frame;

	rec.lr = frame[5];
	rec.pc = frame[6];

	/* Skip s0-s15/FPSCR of an extended frame (stale words when lazily
	 * stacked) and the alignment pad, so only the caller's stack is
	 * searched for return addresses
	 */
	if (rec.current) {
		fp_frame = FPU->FPCAR == psp + 32u;
	} else {
		fp_frame = !(t->arch.mode_exc_return & 0x10u);
	}
	uint32_t start = psp + (fp_frame ? 0x68u : 0x20u) + ((frame[7] & BIT(9)) ? 4u : 0u);

	for (uint32_t a = start; a + 4u <= top && n < STALL_BT_DEPTH; a += 4u) {
		uint32_t w = *(const uint32_t *)a;

		if (is_code(w)) {
			rec.bt[n++] = w;
		}
	}
}

static void snapshot_hw(void)
{
	NRF_SAADC_Type *s = NRF_SAADC;

	rec.saadc = (s->ENABLE & 1u) |
		    ((s->STATUS & 1u) << 1) |
		    ((s->EVENTS_STARTED & 1u) << 2) |
		    ((s->EVENTS_END & 1u) << 3) |
		    ((s->EVENTS_DONE & 1u) << 4) |
		    ((s->EVENTS_RESULTDONE & 1u) << 5) |
		    ((s->EVENTS_STOPPED & 1u) << 6) |
		    ((s->EVENTS_CALIBRATEDONE & 1u) << 7) |
		    ((s->RESULT.AMOUNT & 0xffu) << 8) |
		    ((s->RESULT.MAXCNT & 0xffu) << 16);
	rec.saadc_inten = s->INTEN;
	rec.iser[0] = NVIC->ISER[0];
	rec.iser[1] = NVIC->ISER[1];
	rec.ispr[0] = NVIC->ISPR[0];
	rec.ispr[1] = NVIC->ISPR[1];
	rec.rtc_counter = NRF_RTC1->COUNTER;
	rec.rtc_cc0 = NRF_RTC1->CC[0];
	rec.rtc_inten = NRF_RTC1->INTENSET;
	rec.ppi_chen = NRF_PPI->CHEN;
}

static void trip(enum stall_src src, uint32_t timer_beats)
{
	/* A debugger halt is not a stall: the timer replays the missed beats */
	if (CoreDebug->DHCSR & CoreDebug_DHCSR_C_DEBUGEN_Msk) {
		return;
	}
	if (!atomic_cas(&tripped, 0, 1)) {
		return;
	}

	(void)irq_lock();
	memset(&rec, 0, sizeof(rec));
	rec.up_ms = k_uptime_get_32();
	rec.loops = stall_loops;
	rec.src = (uint8_t)src;
	rec.stage = stall_cur_stage;
	rec.timer_beats = timer_beats;
	snapshot_thread();
	snapshot_hw();
	rec.magic = STALL_MAGIC;

	safety_set_culprit((timer_beats == 0 ? STALL_CULPRIT_NO_TIMER : STALL_CULPRIT) |
			   (rec.stage & 0x0fu));
	/* No time to mute: reset the codecs rather than leave them playing
	 * whatever the floating pins give them
	 */
	board_codecs_kill();
	__DSB();
	NVIC_SystemReset();
}

static void beat(struct k_timer *t)
{
	uint32_t l = stall_loops;

	ARG_UNUSED(t);

	beats = beats + 1u;

	if (!atomic_get(&armed)) {
		return;
	}
	if (l != tmr_seen_loops) {
		tmr_seen_loops = l;
		tmr_idle = 0;
	} else if (++tmr_idle * TIMER_PERIOD_MS >= STALL_MS) {
		trip(STALL_SRC_TIMER, UINT32_MAX);
	}
}

void stall_sof(void)
{
	uint32_t l = stall_loops;

	if (!atomic_get(&armed)) {
		return;
	}
	if (l != sof_seen_loops) {
		sof_seen_loops = l;
		sof_idle_ms = 0;
		sof_beats_at_progress = beats;
	} else if (++sof_idle_ms >= STALL_MS) {
		trip(STALL_SRC_SOF, beats - sof_beats_at_progress);
	}
}

uint32_t stall_timer_beats(void)
{
	return beats;
}

void stall_stage(enum stall_stage s)
{
	uint32_t now = DWT->CYCCNT;
	uint8_t prev = stall_cur_stage;
	uint32_t d = now - stage_t0;

	if (prev < STALL_STAGE_COUNT) {
		stage_cycles[prev] += d;
		if (d > stage_max[prev]) {
			stage_max[prev] = d;
		}
	}
	if (s == STALL_STAGE_WAIT && max_reset) {
		memset(stage_max, 0, sizeof(stage_max));
		max_reset = false;
	}
	stage_t0 = now;
	stall_cur_stage = (uint8_t)s;
}

void stall_get_timing(struct stall_timing *out, bool reset_max)
{
	/* The main thread cannot run in between: no torn 64-bit sums */
	unsigned int key = irq_lock();

	memcpy(out->cycles, stage_cycles, sizeof(out->cycles));
	memcpy(out->max, stage_max, sizeof(out->max));
	if (reset_max) {
		max_reset = true;
	}
	irq_unlock(key);
}

void stall_boot(void)
{
	if (rec.magic == STALL_MAGIC) {
		last = rec;
		have_last = true;
	}
	rec.magic = 0;
}

bool stall_last(struct stall_rec *out)
{
	if (have_last) {
		*out = last;
	}

	return have_last;
}

void stall_start(void)
{
	/* Cycle counter for the stage timing (the thread statistics use it
	 * too; never reset it)
	 */
	CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
	DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
	stage_t0 = DWT->CYCCNT;

	main_tid = k_current_get();
	sof_seen_loops = stall_loops;
	tmr_seen_loops = stall_loops;
	sof_beats_at_progress = beats;

	k_timer_init(&beat_timer, beat, NULL);
	k_timer_start(&beat_timer, K_MSEC(TIMER_PERIOD_MS), K_MSEC(TIMER_PERIOD_MS));
	atomic_set(&armed, 1);
}
