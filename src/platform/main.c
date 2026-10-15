/*
 * SPDX-License-Identifier: MIT
 *
 * SP-1 Mixer: boot policy and bring-up. The main thread then becomes the
 * control loop.
 */

#include <zephyr/kernel.h>
#include <nrf.h>

#include "board_io.h"
#include "control.h"
#include "inputs.h"
#include "leds.h"
#include "safety.h"
#include "stall.h"
#include "store.h"

#if defined(CONFIG_SP1M_AUDIO_OUT)
#include "audio.h"
#endif
#if defined(CONFIG_SP1M_USB)
#include "usb.h"
#endif
#if defined(CONFIG_SP1M_CDC)
#include "console.h"
#endif

/* Turned on by FUNCTION, the watchdog, a soft reset (update, reboot) or a
 * CPU lockup recovery.
 */
#define DELIBERATE_START (POWER_RESETREAS_OFF_Msk | POWER_RESETREAS_DOG_Msk | \
			  POWER_RESETREAS_SREQ_Msk | POWER_RESETREAS_LOCKUP_Msk)

static struct persist loaded;

int main(void)
{
	bool have_state;

	safety_boot();
	stall_boot();
	(void)board_io_init();

	/*
	 * The bootloader starts the application on every power event. With
	 * neither USB power nor a deliberate start (e.g. a battery was
	 * connected) go straight back to sleep.
	 */
	if (!board_usb_power() &&
	    !(safety_reset_reason() & DELIBERATE_START) &&
	    !board_fn_pressed()) {
		board_prepare_system_off();
		safety_system_off();
	}

	(void)leds_init();
	leds_all(30);

	(void)inputs_init();
	(void)store_init();
	have_state = store_load(&loaded);
	safety_wdt_feed();

#if defined(CONFIG_SP1M_USB)
	(void)usb_init();
#endif
#if defined(CONFIG_SP1M_CDC)
	(void)console_init();
#endif
#if defined(CONFIG_SP1M_AUDIO_OUT)
	(void)audio_init();
#endif
	safety_wdt_feed();
	leds_all(0);

	control_run(have_state ? &loaded : NULL);
}
