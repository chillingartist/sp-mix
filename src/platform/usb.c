/*
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <stdlib.h>

#include <arm_acle.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/usb/usbd.h>
#include <zephyr/usb/class/usbd_uac2.h>
#include <hal/nrf_usbd.h>
#include <nrf.h>

#include "console.h"
#include "stall.h"
#include "usb.h"

#if defined(CONFIG_SP1M_AUDIO_OUT)
#include "audio.h"
#endif

/* bMaxPower in 2 mA units: 100 mA, the most an iPad camera adapter allows */
#define MAX_POWER_2MA		50
#define RX_BUF_SIZE		(SP1M_USB_FRAMES_MAX * SP1M_USB_FRAME_BYTES)
#define RX_BUFS			4

USBD_DEVICE_DEFINE(sp1m_usbd, DEVICE_DT_GET(DT_NODELABEL(zephyr_udc0)),
		   CONFIG_SP1M_USB_VID, CONFIG_SP1M_USB_PID);

USBD_DESC_LANG_DEFINE(sp1m_lang);
USBD_DESC_MANUFACTURER_DEFINE(sp1m_mfr, "sp1mixer");
USBD_DESC_PRODUCT_DEFINE(sp1m_product, "SP-1 Mixer");
USBD_DESC_SERIAL_NUMBER_DEFINE(sp1m_sn);
USBD_DESC_CONFIG_DEFINE(sp1m_cfg_desc, "SP-1 Mixer");

/* Bus powered: the battery is charged from VBUS */
USBD_CONFIGURATION_DEFINE(sp1m_config, 0, MAX_POWER_2MA, &sp1m_cfg_desc);

static atomic_t configured;
static atomic_t suspended;
static atomic_t streaming;
static struct usb_stats stats;
static bool have_last_rx;
/* Track input peaks (louder side) for the LED meters, int16 scale */
static atomic_t track_peak[SP1M_TRACKS];

static void stream_reset(void)
{
	atomic_set(&streaming, 0);
	have_last_rx = false;
#if defined(CONFIG_SP1M_AUDIO_OUT)
	audio_usb_active(false);
	audio_usb_reset();
#endif
}

static void bus_active(bool on)
{
#if defined(CONFIG_SP1M_AUDIO_OUT)
	audio_usb_active(on);
#else
	ARG_UNUSED(on);
#endif
}

#if defined(CONFIG_SP1M_UAC2)

#define UAC2_NODE DT_NODELABEL(uac2_mixer)

K_MEM_SLAB_DEFINE_STATIC(rx_slab, RX_BUF_SIZE, RX_BUFS, 4);

static uint16_t last_rx_frame;

/*
 * Two equal reads: a read right as the counter steps at SOF can return a
 * value that is neither the old nor the new frame (run 2 counted three
 * such reads as ~2047 missing frames each, once the audio thread started
 * delaying this callback to arbitrary points in the frame)
 */
static uint16_t frame_no(void)
{
	uint16_t a = (uint16_t)(nrf_usbd_framecntr_get(NRF_USBD) & 0x7ff);

	for (int i = 0; i < 3; i++) {
		uint16_t b = (uint16_t)(nrf_usbd_framecntr_get(NRF_USBD) & 0x7ff);

		if (a == b) {
			break;
		}
		a = b;
	}

	return a;
}

static void uac2_sof(const struct device *dev, void *user_data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(user_data);

	stall_sof();
}

static void uac2_terminal_update(const struct device *dev, uint8_t terminal,
				 bool enabled, bool microframes, void *user_data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(terminal);
	ARG_UNUSED(microframes);
	ARG_UNUSED(user_data);

	atomic_set(&streaming, enabled ? 1 : 0);
	have_last_rx = false;
#if defined(CONFIG_SP1M_AUDIO_OUT)
	audio_set_streaming(enabled);
#endif
}

static void *uac2_get_recv_buf(const struct device *dev, uint8_t terminal,
			       uint16_t size, void *user_data)
{
	void *buf = NULL;

	ARG_UNUSED(dev);
	ARG_UNUSED(terminal);
	ARG_UNUSED(user_data);

	if (size > RX_BUF_SIZE || k_mem_slab_alloc(&rx_slab, &buf, K_NO_WAIT) != 0) {
		stats.no_buffer++;
		return NULL;
	}

	return buf;
}

BUILD_ASSERT(SP1M_IN_CH == 8, "count_packet() reads four words per frame");

/* Per halfword |x|, saturating: -32768 gives 32767 */
static inline uint32_t abs16x2(uint32_t w)
{
	uint32_t n = (uint32_t)__qsub16(0, (int16x2_t)w);

	/* GE per halfword where x >= -x, i.e. x >= 0 */
	(void)__ssub16((int16x2_t)w, (int16x2_t)n);
	return __sel(w, n);
}

/* Per halfword maximum of two non-negative halfword pairs */
static inline uint32_t max16x2(uint32_t a, uint32_t b)
{
	(void)__ssub16((int16x2_t)a, (int16x2_t)b);
	return __sel(a, b);
}

/* Larger steps between packets are counted as jumps, not gaps */
#define GAP_MAX_FRAMES	16

static void count_packet(const int16_t *s, uint16_t size, uint16_t fn)
{
	uint32_t frames = size / SP1M_USB_FRAME_BYTES;

	stats.packets++;
	if (size % SP1M_USB_FRAME_BYTES) {
		stats.partial++;
	}

	switch (frames) {
	case 47:
		stats.frames47++;
		break;
	case 48:
		stats.frames48++;
		break;
	case 49:
		stats.frames49++;
		break;
	default:
		stats.frames_other++;
		break;
	}

	/* Packets are labelled with the frame their callback runs in. One that
	 * is handled a frame late (the audio thread runs above usbd) shows up
	 * as a gap followed by two packets in the same frame: take the gap
	 * back then, and count the late delivery instead
	 */
	if (have_last_rx) {
		uint16_t d = (uint16_t)((fn - last_rx_frame) & 0x7ff);

		if (d == 0) {
			stats.late_rx++;
			if (stats.gaps > 0) {
				stats.gaps--;
			}
		} else if (d > GAP_MAX_FRAMES) {
			/* A host pause, or a bad frame number: not counted as
			 * missing packets, so that one event cannot hide in
			 * thousands of 'gaps'
			 */
			stats.jumps++;
		} else if (d > 1) {
			stats.gaps += d - 1u;
		}
	}
	last_rx_frame = fn;
	have_last_rx = true;

	/* Per-channel maxima, one pass over the frames: this runs in the
	 * cooperative usbd thread for every packet. Two channels per word
	 * with the DSP extension's halfword SIMD: saturating |x| (-32768
	 * reads as 32767, like the clamp below) and a running maximum.
	 */
	uint32_t p01 = 0, p23 = 0, p45 = 0, p67 = 0;
	const uint32_t *w = (const uint32_t *)s;

	for (uint32_t f = 0; f < frames; f++, w += SP1M_IN_CH / 2) {
		p01 = max16x2(abs16x2(w[0]), p01);
		p23 = max16x2(abs16x2(w[1]), p23);
		p45 = max16x2(abs16x2(w[2]), p45);
		p67 = max16x2(abs16x2(w[3]), p67);
	}

	int32_t m0 = (int32_t)(p01 & 0xffff), m1 = (int32_t)(p01 >> 16);
	int32_t m2 = (int32_t)(p23 & 0xffff), m3 = (int32_t)(p23 >> 16);
	int32_t m4 = (int32_t)(p45 & 0xffff), m5 = (int32_t)(p45 >> 16);
	int32_t m6 = (int32_t)(p67 & 0xffff), m7 = (int32_t)(p67 >> 16);
	const int32_t m[SP1M_IN_CH] = { m0, m1, m2, m3, m4, m5, m6, m7 };

	for (int ch = 0; ch < SP1M_IN_CH; ch++) {
		int16_t a = (int16_t)MIN(m[ch], INT16_MAX);

		if (a > stats.peak[ch]) {
			stats.peak[ch] = a;
		}
	}
	for (int t = 0; t < SP1M_TRACKS; t++) {
		atomic_val_t a = MIN(MAX(m[2 * t], m[2 * t + 1]), INT16_MAX);

		/* Racing with the clear in usb_take_track_peaks() only
		 * moves this packet's peak into the next window
		 */
		if (a > atomic_get(&track_peak[t])) {
			atomic_set(&track_peak[t], a);
		}
	}
}

static void uac2_data_recv(const struct device *dev, uint8_t terminal,
			   void *buf, uint16_t size, void *user_data)
{
	uint16_t fn = frame_no();

	ARG_UNUSED(dev);
	ARG_UNUSED(terminal);
	ARG_UNUSED(user_data);

	if (size == 0 && atomic_get(&streaming)) {
		/* The host keeps the interface open but sends nothing. A
		 * delivered packet all the same: no gap when data resumes
		 */
		stats.zero_length++;
		last_rx_frame = fn;
		have_last_rx = true;
	} else if (size > 0 && atomic_get(&streaming)) {
		uint32_t t0 = DWT->CYCCNT;
		uint32_t d;

		count_packet(buf, size, fn);
#if defined(CONFIG_SP1M_AUDIO_OUT)
		audio_usb_rx(buf, size, fn);
#endif
		d = DWT->CYCCNT - t0;
		stats.rx_calls++;
		stats.rx_cycles += d;
		if (d > stats.rx_max) {
			stats.rx_max = d;
		}
	}

	k_mem_slab_free(&rx_slab, buf);
}

static const struct uac2_ops uac2_ops = {
	.sof_cb = uac2_sof,
	.terminal_update_cb = uac2_terminal_update,
	.get_recv_buf = uac2_get_recv_buf,
	.data_recv_cb = uac2_data_recv,
};

#endif /* CONFIG_SP1M_UAC2 */

static void msg_cb(struct usbd_context *const ctx, const struct usbd_msg *msg)
{
	switch (msg->type) {
	case USBD_MSG_VBUS_READY:
		if (usbd_can_detect_vbus(ctx)) {
			(void)usbd_enable(ctx);
		}
		break;
	case USBD_MSG_VBUS_REMOVED:
		atomic_set(&configured, 0);
		stream_reset();
		if (usbd_can_detect_vbus(ctx)) {
			(void)usbd_disable(ctx);
		}
		break;
	case USBD_MSG_RESET:
		atomic_set(&configured, 0);
		stream_reset();
		break;
	case USBD_MSG_CONFIGURATION:
		atomic_set(&configured, msg->status != 0);
		bus_active(msg->status != 0 && !atomic_get(&suspended));
		break;
	case USBD_MSG_SUSPEND:
		atomic_set(&suspended, 1);
		stream_reset();
		break;
	case USBD_MSG_RESUME:
		atomic_set(&suspended, 0);
		bus_active(atomic_get(&configured) != 0);
		break;
#if defined(CONFIG_SP1M_CDC)
	case USBD_MSG_CDC_ACM_CONTROL_LINE_STATE:
		console_line_state_changed();
		break;
#endif
	default:
		break;
	}
}

int usb_init(void)
{
	int err;

#if defined(CONFIG_SP1M_UAC2)
	usbd_uac2_set_ops(DEVICE_DT_GET(UAC2_NODE), &uac2_ops, NULL);
#endif

	err = usbd_add_descriptor(&sp1m_usbd, &sp1m_lang);
	err |= usbd_add_descriptor(&sp1m_usbd, &sp1m_mfr);
	err |= usbd_add_descriptor(&sp1m_usbd, &sp1m_product);
	err |= usbd_add_descriptor(&sp1m_usbd, &sp1m_sn);
	if (err) {
		return -EIO;
	}

	err = usbd_add_configuration(&sp1m_usbd, USBD_SPEED_FS, &sp1m_config);
	if (err) {
		return err;
	}

	err = usbd_register_all_classes(&sp1m_usbd, USBD_SPEED_FS, 1, NULL);
	if (err) {
		return err;
	}

	/* Composite device: class information lives in the IADs */
	usbd_device_set_code_triple(&sp1m_usbd, USBD_SPEED_FS,
				    USB_BCC_MISCELLANEOUS, 0x02, 0x01);
	usbd_device_set_bcd_device(&sp1m_usbd, CONFIG_SP1M_USB_BCD_DEVICE);
	usbd_self_powered(&sp1m_usbd, false);

	err = usbd_msg_register_cb(&sp1m_usbd, msg_cb);
	if (err) {
		return err;
	}

	err = usbd_init(&sp1m_usbd);
	if (err) {
		return err;
	}

	if (!usbd_can_detect_vbus(&sp1m_usbd)) {
		return usbd_enable(&sp1m_usbd);
	}

	return 0;
}

bool usb_configured(void)
{
	return atomic_get(&configured) != 0;
}

bool usb_streaming(void)
{
	return atomic_get(&streaming) != 0;
}

void usb_get_stats(struct usb_stats *out)
{
	*out = stats;
	out->configured = usb_configured();
	out->suspended = atomic_get(&suspended) != 0;
	out->streaming = usb_streaming();
	memset(stats.peak, 0, sizeof(stats.peak));
	stats.rx_calls = 0;
	stats.rx_cycles = 0;
	stats.rx_max = 0;
}

void usb_take_track_peaks(float peak[SP1M_TRACKS])
{
	for (int t = 0; t < SP1M_TRACKS; t++) {
		peak[t] = (float)atomic_clear(&track_peak[t]) / 32767.0f;
	}
}

void usb_reset_stats(void)
{
	memset(&stats, 0, sizeof(stats));
}

void usb_shutdown(void)
{
	stream_reset();
	(void)usbd_disable(&sp1m_usbd);
}
