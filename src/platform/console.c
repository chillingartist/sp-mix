/*
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/drivers/uart.h>
#include <zephyr/toolchain.h>
#include <zephyr/kernel.h>
#include <zephyr/spinlock.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/sys/ring_buffer.h>
#include <nrf.h>

#include "board_io.h"
#include "buttons.h"
#include "cli.h"
#include "console.h"
#include "control.h"
#include "inputs.h"
#include "safety.h"
#include "stall.h"
#include "usb.h"

#if defined(CONFIG_SP1M_AUDIO_OUT)
#include "audio.h"
#include "codec.h"
#endif

static const struct device *const cdc = DEVICE_DT_GET(DT_NODELABEL(cdc_acm_uart0));

RING_BUF_DECLARE(rx_rb, 128);
RING_BUF_DECLARE(tx_rb, 2048);
static struct k_spinlock tx_lock;
static struct k_spinlock rx_lock;
static K_SEM_DEFINE(rx_sem, 0, 1);
static atomic_t dtr;
static bool tx_idle = true;	/* TX interrupt off, needs a kick (tx_lock) */
static bool tx_batch;		/* console thread: collect, then kick once */
static bool watch;

static struct cli cli;

K_THREAD_STACK_DEFINE(console_stack, 2048);
static struct k_thread console_thread_data;

static void uart_isr(const struct device *dev, void *user_data)
{
	ARG_UNUSED(user_data);

	while (uart_irq_update(dev) && uart_irq_is_pending(dev)) {
		if (uart_irq_rx_ready(dev)) {
			uint8_t buf[32];
			int n = uart_fifo_read(dev, buf, sizeof(buf));

			if (n > 0) {
				k_spinlock_key_t key = k_spin_lock(&rx_lock);

				ring_buf_put(&rx_rb, buf, (uint32_t)n);
				k_spin_unlock(&rx_lock, key);
				k_sem_give(&rx_sem);
			}
		}

		if (uart_irq_tx_ready(dev)) {
			uint8_t *data;
			uint32_t n;
			int sent;
			k_spinlock_key_t key = k_spin_lock(&tx_lock);

			n = ring_buf_get_claim(&tx_rb, &data, 64);
			k_spin_unlock(&tx_lock, key);

			if (n == 0) {
				uart_irq_tx_disable(dev);
				/* Data that came in meanwhile needs the next
				 * out() to kick, or another round here
				 */
				key = k_spin_lock(&tx_lock);
				tx_idle = ring_buf_is_empty(&tx_rb);
				k_spin_unlock(&tx_lock, key);
				if (!tx_idle) {
					uart_irq_tx_enable(dev);
				}
				continue;
			}

			sent = uart_fifo_fill(dev, data, (int)n);
			key = k_spin_lock(&tx_lock);
			(void)ring_buf_get_finish(&tx_rb, sent > 0 ? (uint32_t)sent : 0);
			k_spin_unlock(&tx_lock, key);
		}
	}
}

static void out(void *ctx, const char *s)
{
	size_t len = strlen(s);
	k_spinlock_key_t key;
	bool kick;

	ARG_UNUSED(ctx);

	if (!atomic_get(&dtr)) {
		return;
	}

	/* Kick the transmitter only when it went idle: every kick is a
	 * round trip through the cooperative system work queue (CDC ACM)
	 */
	key = k_spin_lock(&tx_lock);
	ring_buf_put(&tx_rb, (const uint8_t *)s, (uint32_t)len);
	kick = tx_idle && (!tx_batch || ring_buf_space_get(&tx_rb) <
					 ring_buf_capacity_get(&tx_rb) / 2u);
	if (kick) {
		tx_idle = false;
	}
	k_spin_unlock(&tx_lock, key);
	if (kick) {
		uart_irq_tx_enable(cdc);
	}
}

/* A multi-line report goes out in one go (kicked early if the buffer
 * fills up halfway)
 */
static void tx_batch_begin(void)
{
	tx_batch = true;
}

static void tx_batch_end(void)
{
	k_spinlock_key_t key = k_spin_lock(&tx_lock);
	bool kick = tx_idle && !ring_buf_is_empty(&tx_rb);

	tx_batch = false;
	if (kick) {
		tx_idle = false;
	}
	k_spin_unlock(&tx_lock, key);
	if (kick) {
		uart_irq_tx_enable(cdc);
	}
}

static __printf_like(1, 2) void outf(const char *fmt, ...)
{
	char buf[256];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	out(NULL, buf);
}

void console_line_state_changed(void)
{
	uint32_t v = 0;

	if (uart_line_ctrl_get(cdc, UART_LINE_CTRL_DTR, &v) == 0) {
		atomic_set(&dtr, v ? 1 : 0);
	}
	if (v) {
		/* Port (re)opened: the next output kicks the transmitter
		 * again, whatever state it was left in (an extra kick is
		 * harmless)
		 */
		k_spinlock_key_t key = k_spin_lock(&tx_lock);

		tx_idle = true;
		k_spin_unlock(&tx_lock, key);
	}
}

#if defined(CONFIG_SP1M_AUDIO_OUT)
/* Signed value with two decimals, without float printf support */
static void fmt_centi(char *buf, size_t len, float v)
{
	int32_t c = (int32_t)lroundf(v * 100.0f);
	uint32_t a = (uint32_t)(c < 0 ? -c : c);

	snprintf(buf, len, "%c%u.%02u", c < 0 ? '-' : '+', a / 100u, a % 100u);
}
#endif

static int peak_db(int16_t peak)
{
	if (peak <= 0) {
		return -99;
	}

	return (int)lroundf(20.0f * log10f((float)peak / 32767.0f));
}

#define MAX_THREADS 16

struct thread_use {
	k_tid_t tid;
	const char *name;
	uint64_t cycles;
	uint64_t delta;
	size_t unused;
};

static struct thread_use threads[MAX_THREADS];
static size_t nthreads;
static uint64_t last_total;
static int64_t last_ticks;

static void collect_thread(const struct k_thread *cthread, void *user_data)
{
	k_tid_t t = (k_tid_t)cthread;
	k_thread_runtime_stats_t rs;
	struct thread_use *u = NULL;
	const bool *stacks = user_data;

	for (size_t i = 0; i < nthreads; i++) {
		if (threads[i].tid == t) {
			u = &threads[i];
			break;
		}
	}
	if (u == NULL) {
		if (nthreads == MAX_THREADS) {
			return;
		}
		u = &threads[nthreads++];
		u->tid = t;
		u->cycles = 0;
	}

	u->name = k_thread_name_get(t);
	if (k_thread_runtime_stats_get(t, &rs) == 0) {
		u->delta = rs.execution_cycles - u->cycles;
		u->cycles = rs.execution_cycles;
	}
	/* Scans the whole stack: only for 'threads', not every status */
	if (*stacks && k_thread_stack_space_get(t, &u->unused) != 0) {
		u->unused = 0;
	}
}

/* Cycles every thread was awake since the previous call */
static uint64_t sample_threads(bool stacks)
{
	k_thread_runtime_stats_t all;
	uint64_t elapsed = 0;

	k_thread_foreach_unlocked(collect_thread, &stacks);
	if (k_thread_runtime_stats_all_get(&all) == 0) {
		elapsed = all.execution_cycles - last_total;
		last_total = all.execution_cycles;
	}

	return elapsed;
}

static unsigned int pct(uint64_t part, uint64_t whole)
{
	return whole ? (unsigned int)((part * 100u + whole / 2u) / whole) : 0u;
}

/*
 * Share of wall-clock time per thread since the previous call, in percent.
 * The thread statistics count DWT cycles, which stop while the CPU sleeps
 * in WFI, so they only add up to the awake time; the rest is "sleep".
 */
static void print_threads(bool stacks)
{
	uint64_t awake = sample_threads(stacks);
	int64_t now = k_uptime_ticks();
	uint64_t wall = (uint64_t)(now - last_ticks) * SystemCoreClock /
			CONFIG_SYS_CLOCK_TICKS_PER_SEC;

	last_ticks = now;
	if (wall < awake) {
		wall = awake;
	}

	outf("threads");
	for (size_t i = 0; i < nthreads; i++) {
		const char *n = threads[i].name ? threads[i].name : "?";

		if (stacks) {
			outf(" %s %u%%/%uB", n, pct(threads[i].delta, wall),
			     (unsigned int)threads[i].unused);
		} else {
			outf(" %s %u", n, pct(threads[i].delta, wall));
		}
	}
	outf(stacks ? " sleep %u%%\r\n" : " sleep %u\r\n", pct(wall - awake, wall));
}

/* Where the control loop was stuck before the previous reset */
static void print_stall(void)
{
	struct stall_rec r;

	if (!stall_last(&r)) {
		return;
	}

	outf("stall src %s stage %u up %u loops %u state 0x%02x run %u "
	     "wait 0x%08x to %d tmr %d pc 0x%08x lr 0x%08x\r\n",
	     r.src == STALL_SRC_SOF ? "sof" : "timer", r.stage, r.up_ms,
	     r.loops, r.state, r.current, r.wait, r.timeout,
	     (int)r.timer_beats, r.pc, r.lr);
	outf("stall bt");
	for (int i = 0; i < STALL_BT_DEPTH; i++) {
		outf(" 0x%08x", r.bt[i]);
	}
	outf("\r\n");
	outf("stall hw saadc 0x%08x inten 0x%08x iser 0x%08x 0x%08x "
	     "ispr 0x%08x 0x%08x rtc %u cc0 %u inten 0x%08x ppi 0x%08x\r\n",
	     r.saadc, r.saadc_inten, r.iser[0], r.iser[1], r.ispr[0],
	     r.ispr[1], r.rtc_counter, r.rtc_cc0, r.rtc_inten, r.ppi_chen);
}

/*
 * Control loop: iterations, ADC scan health, and the average and longest
 * time per iteration in each stage since the previous status.
 */
static void print_main(void)
{
	static const char *const names[] = {
		[STALL_STAGE_WAIT] = "wait", [STALL_STAGE_INPUTS] = "in",
		[STALL_STAGE_OUTPUTS] = "out", [STALL_STAGE_UI] = "ui",
		[STALL_STAGE_MIX] = "mix", [STALL_STAGE_LEDS] = "leds",
		[STALL_STAGE_CMDS] = "cmds", [STALL_STAGE_PERSIST] = "persist",
		[STALL_STAGE_STATUS] = "status",
	};
	static uint64_t prev_cycles[ARRAY_SIZE(names)];
	static uint32_t prev_loops;
	struct stall_timing t;
	struct inputs_stats a;
	uint32_t loops = stall_loops;
	uint32_t n = loops - prev_loops;
	const uint32_t mhz = SystemCoreClock / 1000000u;

	inputs_get_stats(&a);
	outf("main loops %u stage %u tmr %u adc ok %u nostart %u timeout %u "
	     "short %u busy %u pcycle %u\r\n", loops, stall_cur_stage,
	     stall_timer_beats(), a.scans, a.no_start, a.timeouts,
	     a.short_scans, a.not_ready, a.power_cycles);

	stall_get_timing(&t, true);
	outf("main us");
	for (size_t i = STALL_STAGE_WAIT; i < ARRAY_SIZE(names); i++) {
		uint64_t d = t.cycles[i] - prev_cycles[i];

		prev_cycles[i] = t.cycles[i];
		outf(" %s %u/%u", names[i], n ? (unsigned int)(d / n / mhz) : 0u,
		     t.max[i] / mhz);
	}
	outf("\r\n");
	prev_loops = loops;
}

#if defined(CONFIG_SP1M_AUDIO_OUT)
/* Codec bring-up results: what failed where, and the raw status bytes */
static void print_codec(void)
{
	struct codec_status s;

	codec_get_status(&s);
	outf("codec cs42 %s step %u tries %u osc 0x%02x tas %s step %u spk %d "
	     "jack %s0x%02x i2cerr %u\r\n",
	     s.cs42_ok ? "ok" : s.cs42_answers ? "partial" : "none", s.cs42_step,
	     s.cs42_tries, s.osc_stat, s.tas_ok ? "ok" : "fail", s.tas_step,
	     s.speaker_on, s.jack_valid ? "" : "?", s.jack, s.i2c_errors);
}

static bool parse_hex(const char *s, uint32_t max, uint32_t *out)
{
	char *end;
	unsigned long v = strtoul(s, &end, 16);

	if (*s == '\0' || *end != '\0' || v > max) {
		return false;
	}
	*out = (uint32_t)v;

	return true;
}

static void cmd_codec(struct cli *c, int argc, char **argv)
{
	uint32_t reg, page;
	uint8_t v;

	if (argc == 1) {
		print_codec();
	} else if (argc == 3 && strcmp(argv[1], "cs") == 0 &&
		   parse_hex(argv[2], 0xffff, &reg)) {
		if (codec_cs42_read((uint16_t)reg, &v) == 0) {
			outf("cs42 0x%04x = 0x%02x\r\n", reg, v);
		} else {
			out(NULL, "error: no answer\r\n");
		}
	} else if (argc == 4 && strcmp(argv[1], "tas") == 0 &&
		   parse_hex(argv[2], 0xff, &page) && parse_hex(argv[3], 0x7f, &reg)) {
		if (codec_tas_read((uint8_t)page, (uint8_t)reg, &v) == 0) {
			outf("tas 0x%02x/0x%02x = 0x%02x\r\n", page, reg, v);
		} else {
			out(NULL, "error: no answer\r\n");
		}
	} else {
		out(NULL, "usage: codec | codec cs 1b77 | codec tas 0 3f\r\n");
	}
}
#endif

static void print_status_lines(void)
{
	struct safety_fault f;
	struct ctrl_status c;
	struct usb_stats u;

	safety_last_fault(&f);
	control_get_status(&c);
	usb_get_stats(&u);

	outf("sp1mixer %s up %us reset 0x%08x fault %s",
	     CONFIG_SP1M_VARIANT, k_uptime_get_32() / 1000u,
	     safety_reset_reason(), f.valid ? "yes" : "none");
	if (f.valid) {
		outf(" reason %u pc 0x%08x", f.reason, f.pc);
	}
	outf(" culprit 0x%02x", f.culprit);
	if (f.culprit) {
		outf(" kept %s", f.kept == (SAFETY_KEPT_REG | SAFETY_KEPT_RAM) ? "reg+ram" :
				 f.kept == SAFETY_KEPT_REG ? "reg" : "ram");
	}
	outf(" fw %s\r\n", SP1M_GIT);
	print_stall();

	const uint32_t mhz = SystemCoreClock / 1000000u;

	/* rx: average/longest time per received packet since the last status */
	outf("usb cfg %d susp %d stream %d pkts %u n47 %u n48 %u n49 %u other %u "
	     "zlp %u part %u gaps %u late %u jumps %u nobuf %u rx %u/%u\r\n",
	     u.configured, u.suspended, u.streaming, u.packets, u.frames47,
	     u.frames48, u.frames49, u.frames_other, u.zero_length, u.partial,
	     u.gaps, u.late_rx, u.jumps, u.no_buffer,
	     u.rx_calls ? (unsigned int)(u.rx_cycles / u.rx_calls / mhz) : 0u,
	     u.rx_max / mhz);

	outf("peak dBFS");
	for (int ch = 0; ch < SP1M_IN_CH; ch++) {
		outf(" %d", peak_db(u.peak[ch]));
	}
	outf("\r\n");

#if defined(CONFIG_SP1M_AUDIO_OUT)
	struct audio_stats a;
	static const char *const states[] = { "idle", "preroll", "run", "fade" };
	char ppm[16], ff[16], lin[16];

	audio_get_stats(&a);
	fmt_centi(ppm, sizeof(ppm), a.ppm);
	fmt_centi(ff, sizeof(ff), a.ff_ppm);
	fmt_centi(lin, sizeof(lin), a.fill_lin);
	outf("audio %s fill %u lin %s ppm %s ff %s%s under %u over %u drop %u "
	     "i2s %u/%u sofrej %u clip %u blk %u/%u us\r\n",
	     a.state < ARRAY_SIZE(states) ? states[a.state] : "?", a.fill, lin,
	     ppm, ff, a.ff_valid ? "" : "?", a.underruns, a.overruns,
	     a.rx_dropped, a.i2s_errors, a.i2s_stalls, a.sof_rejects, a.clipped,
	     a.block_us_avg, a.block_us_max);
	/* blk above: playing blocks only. slow: over one block period; late:
	 * blocks that started 1, 2 or 3+ periods late; gapmax: longest time
	 * between block starts (one period = 1333 us)
	 */
	outf("audio2 run %d init %d blocks %u restartfail %u last %d prio %s "
	     "playing %u slow %u late %u/%u/%u gapmax %u sofrel %u\r\n",
	     a.running, a.init_err, a.blocks, a.i2s_start_errors, a.i2s_last_err,
	     a.coop ? "coop" : "preempt", a.run_blocks, a.slow_blocks,
	     a.late[0], a.late[1], a.late[2], a.gap_max_us, a.sof_relabels);
	if (a.err_kind != AUDIO_ERR_NONE) {
		static const char *const kinds[] = { "none", "late", "stall", "start" };

		outf("audioerr kind %s at %u.%03u state %s fill %u gap %u us %u code %d\r\n",
		     a.err_kind < ARRAY_SIZE(kinds) ? kinds[a.err_kind] : "?",
		     a.err_up_ms / 1000u, a.err_up_ms % 1000u,
		     a.err_state < ARRAY_SIZE(states) ? states[a.err_state] : "?",
		     a.err_fill, a.err_gap_us, a.err_us, a.err_code);
	}
	print_codec();
#endif

	outf("ctrl vol %u %u %u %u bal %d %d %d %d mute 0x%x solo 0x%x master %d "
	     "spk %s/%d hp %s chg %d%s slots 0x%x dirty %d writes %u/%u err\r\n",
	     c.cur.vol[0], c.cur.vol[1], c.cur.vol[2], c.cur.vol[3],
	     c.cur.bal[0], c.cur.bal[1], c.cur.bal[2], c.cur.bal[3],
	     c.cur.mute, c.cur.solo, c.cur.master_db,
	     c.speaker_forced ? "on" : c.cur.spk_mode == SPK_AUTO ? "auto" : "off",
	     c.speaker_on,
	     !IS_ENABLED(CONFIG_SP1M_AUDIO_OUT) ? "-" :
	     c.hp_known ? (c.hp_present ? "1" : "0") : "?",
	     c.charging, c.charge_off ? " (off)" : "",
	     c.slot_valid, c.dirty, c.store_writes, c.store_errors);

	print_main();
	print_threads(false);
}

static void print_status(void)
{
	tx_batch_begin();
	print_status_lines();
	tx_batch_end();
}

static void cmd_status(struct cli *c, int argc, char **argv)
{
	print_status();
}

static void cmd_threads(struct cli *c, int argc, char **argv)
{
	/* CPU since the last status/threads call, and unused stack bytes */
	print_threads(true);
}

static void cmd_adc(struct cli *c, int argc, char **argv)
{
	struct ctrl_status s;

	control_get_status(&s);
	outf("ladder1 %d ladder2 %d -> 0x%03x fn %d faders raw %d %d %d %d "
	     "pos %u %u %u %u battery %d usb %d\r\n",
	     s.raw.ladder1, s.raw.ladder2,
	     buttons_from_ladders(s.raw.ladder1, s.raw.ladder2) & 0x3ff,
	     s.raw.fn, s.raw.fader[0], s.raw.fader[1], s.raw.fader[2],
	     s.raw.fader[3], s.fader[0], s.fader[1], s.fader[2], s.fader[3],
	     s.raw.battery, board_usb_power());
}

static void post(uint8_t type, int8_t arg)
{
	struct ctrl_cmd cmd = { .type = type, .arg = arg };

	if (control_post(&cmd) != 0) {
		out(NULL, "error: busy\r\n");
	} else {
		out(NULL, "ok\r\n");
	}
}

static void cmd_scene(struct cli *c, int argc, char **argv)
{
	int slot;

	if (!cli_parse_int(argv[2], 1, SCENE_COUNT, &slot)) {
		out(NULL, "error: slot 1-4\r\n");
	} else if (strcmp(argv[1], "save") == 0) {
		post(CTRL_SCENE_SAVE, (int8_t)(slot - 1));
	} else if (strcmp(argv[1], "load") == 0) {
		post(CTRL_SCENE_LOAD, (int8_t)(slot - 1));
	} else {
		out(NULL, "error: save or load\r\n");
	}
}

#if defined(CONFIG_SP1M_AUDIO_OUT)
static void cmd_audio(struct cli *c, int argc, char **argv)
{
	if (strcmp(argv[1], "coop") == 0) {
		audio_set_coop(true);
	} else if (strcmp(argv[1], "preempt") == 0) {
		audio_set_coop(false);
	} else {
		out(NULL, "error: coop or preempt\r\n");
		return;
	}
	/* So that a record holds one mode only */
	usb_reset_stats();
	audio_reset_stats();
	out(NULL, "ok, counters cleared\r\n");
}
#endif

static void cmd_spk(struct cli *c, int argc, char **argv)
{
	if (strcmp(argv[1], "auto") == 0) {
		post(CTRL_SPEAKER, SPK_AUTO);
	} else if (strcmp(argv[1], "off") == 0) {
		post(CTRL_SPEAKER, SPK_OFF);
	} else if (strcmp(argv[1], "on") == 0) {
#if defined(CONFIG_SP1M_AUDIO_OUT)
		/* Not saved: for checking the speaker whatever the jack says */
		post(CTRL_SPEAKER_FORCE, 1);
#else
		out(NULL, "error: no audio output in this build\r\n");
#endif
	} else {
		out(NULL, "error: auto, off or on\r\n");
	}
}

static void cmd_vol(struct cli *c, int argc, char **argv)
{
	int db;

	if (strcmp(argv[1], "off") == 0) {
		post(CTRL_MASTER, MASTER_DB_OFF);
	} else if (cli_parse_int(argv[1], MASTER_DB_MIN, MASTER_DB_MAX, &db)) {
		post(CTRL_MASTER, (int8_t)db);
	} else {
		outf("error: %d..%d or off\r\n", MASTER_DB_MIN, MASTER_DB_MAX);
	}
}

static void cmd_charge(struct cli *c, int argc, char **argv)
{
	if (strcmp(argv[1], "on") == 0) {
		post(CTRL_CHARGE, 1);
	} else if (strcmp(argv[1], "off") == 0) {
		post(CTRL_CHARGE, 0);
	} else {
		out(NULL, "error: on or off\r\n");
	}
}

static void cmd_watch(struct cli *c, int argc, char **argv)
{
	watch = strcmp(argv[1], "on") == 0;
	out(NULL, "ok\r\n");
}

static void cmd_clear(struct cli *c, int argc, char **argv)
{
	usb_reset_stats();
#if defined(CONFIG_SP1M_AUDIO_OUT)
	audio_reset_stats();
#endif
	out(NULL, "ok\r\n");
}

static void cmd_dfu(struct cli *c, int argc, char **argv)
{
	out(NULL, "rebooting to the bootloader; if the updater does not see "
		  "the SP-1, hold Track 1 + Track 4 while replugging USB\r\n");
	k_msleep(100);
	post(CTRL_BOOTLOADER, 0);
}

static void cmd_reboot(struct cli *c, int argc, char **argv)
{
	out(NULL, "rebooting\r\n");
	k_msleep(100);
	/* The control thread mutes and powers the codecs down first */
	post(CTRL_REBOOT, 0);
}

static void cmd_off(struct cli *c, int argc, char **argv)
{
	post(CTRL_POWER_OFF, 0);
}

static void cmd_selftest(struct cli *c, int argc, char **argv)
{
	if (strcmp(argv[1], "stall") == 0) {
		out(NULL, "control loop sleeps 3 s: expect a soft reset with a "
			  "stall record (stage 7)\r\n");
		post(CTRL_SELFTEST, 0);
	} else if (strcmp(argv[1], "wdt") == 0) {
		out(NULL, "watchdog no longer fed: expect a watchdog reset; "
			  "culprit 0x4a shows what survived\r\n");
		post(CTRL_SELFTEST, 1);
	} else {
		out(NULL, "error: stall or wdt\r\n");
	}
}

static void cmd_help(struct cli *c, int argc, char **argv);

static const struct cli_cmd cmds[] = {
	{ "status", "", 0, 0, cmd_status },
	{ "watch", "on|off  (status every second)", 1, 1, cmd_watch },
	{ "adc", "", 0, 0, cmd_adc },
	{ "threads", "(CPU % and free stack per thread)", 0, 0, cmd_threads },
	{ "clear", "(reset counters)", 0, 0, cmd_clear },
	{ "scene", "save|load <1-4>", 2, 2, cmd_scene },
	{ "spk", "auto|off|on  (on: not saved, ignores the jack)", 1, 1, cmd_spk },
#if defined(CONFIG_SP1M_AUDIO_OUT)
	{ "audio", "coop|preempt  (audio thread above or below USB; not saved)", 1, 1,
	  cmd_audio },
#endif
	{ "vol", "<-60..0>|off  (master volume, dB)", 1, 1, cmd_vol },
#if defined(CONFIG_SP1M_AUDIO_OUT)
	{ "codec", "[cs <reg hex> | tas <page hex> <reg hex>]", 0, 3, cmd_codec },
#endif
	{ "charge", "on|off", 1, 1, cmd_charge },
	{ "dfu", "(reboot into the bootloader)", 0, 0, cmd_dfu },
	{ "reboot", "", 0, 0, cmd_reboot },
	{ "off", "(power off)", 0, 0, cmd_off },
	{ "selftest", "stall|wdt  (reset on purpose, check the reset record)", 1, 1,
	  cmd_selftest },
	{ "help", "", 0, 0, cmd_help },
};

static void cmd_help(struct cli *c, int argc, char **argv)
{
	for (size_t i = 0; i < ARRAY_SIZE(cmds); i++) {
		outf("%s %s\r\n", cmds[i].name, cmds[i].help);
	}
}

static void console_thread(void *a, void *b, void *c)
{
	uint32_t next_watch = 0;

	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	for (;;) {
		uint8_t buf[32];
		uint32_t n;

		(void)k_sem_take(&rx_sem, K_MSEC(200));
		for (;;) {
			k_spinlock_key_t key = k_spin_lock(&rx_lock);

			n = ring_buf_get(&rx_rb, buf, sizeof(buf));
			k_spin_unlock(&rx_lock, key);
			if (n == 0) {
				break;
			}
			cli_input(&cli, (const char *)buf, n);
		}

		if (watch && (int32_t)(k_uptime_get_32() - next_watch) >= 0) {
			next_watch = k_uptime_get_32() + 1000;
			print_status();
		}
	}
}

int console_init(void)
{
	if (!device_is_ready(cdc)) {
		return -ENODEV;
	}

	cli_init(&cli, cmds, ARRAY_SIZE(cmds), out, NULL);
	uart_irq_callback_set(cdc, uart_isr);
	uart_irq_rx_enable(cdc);

	k_thread_create(&console_thread_data, console_stack,
			K_THREAD_STACK_SIZEOF(console_stack), console_thread,
			NULL, NULL, NULL, K_PRIO_PREEMPT(12), 0, K_NO_WAIT);
	k_thread_name_set(&console_thread_data, "console");

	return 0;
}
