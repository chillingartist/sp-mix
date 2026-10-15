/*
 * SPDX-License-Identifier: MIT
 */

#include "test.h"

int test_failures;
int test_checks;

#define TESTS(X)				\
	X(test_ring_basic)			\
	X(test_ring_wrap_and_full)		\
	X(test_ring_peek_consume)		\
	X(test_mixer_balance_law)		\
	X(test_mixer_mute_solo)			\
	X(test_mixer_process)			\
	X(test_mixer_dc_block)			\
	X(test_mixer_dc_and_clip)		\
	X(test_mixer_soft_clip)			\
	X(test_asrc_accounting)			\
	X(test_asrc_quality)			\
	X(test_asrc_i24)			\
	X(test_rate_est_locked_host)		\
	X(test_rate_est_unlocked_host)		\
	X(test_rate_est_late_blocks)		\
	X(test_rate_est_output_restart)		\
	X(test_rate_est_rejects_bad_sof)	\
	X(test_rate_est_sof_pairing)		\
	X(test_buttons_decode)			\
	X(test_buttons_debounce)		\
	X(test_fader_filter)			\
	X(test_fader_taper_and_pickup)		\
	X(test_scene_roundtrip)			\
	X(test_ui_mute_solo)			\
	X(test_ui_balance)			\
	X(test_ui_supply_sag)			\
	X(test_ui_sag_before_debounce)		\
	X(test_ui_inputs_lost)			\
	X(test_ui_recovery_and_power)		\
	X(test_ui_scenes)			\
	X(test_ui_master_volume)		\
	X(test_ui_volume_safety)		\
	X(test_ui_persist)			\
	X(test_cli)

#define DECLARE(fn) void fn(void);
TESTS(DECLARE)

#define RUN(fn)						\
	do {						\
		int before = test_failures;		\
		fn();					\
		printf("%s %s\n", test_failures == before ? "ok  " : "FAIL", #fn); \
	} while (0);

int main(void)
{
	TESTS(RUN)
	printf("%d checks, %d failures\n", test_checks, test_failures);

	return test_failures == 0 ? 0 : 1;
}
