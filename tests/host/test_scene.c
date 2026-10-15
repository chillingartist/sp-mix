/*
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

#include "scene.h"
#include "test.h"

TEST(test_scene_roundtrip)
{
	struct persist a, b;
	uint8_t buf[PERSIST_ENCODED_MAX];
	size_t n;

	persist_default(&a);
	a.current.vol[2] = 777;
	a.current.bal[1] = -42;
	a.current.mute = 0x5;
	a.current.solo = 0x8;
	a.current.master_db = MASTER_DB_OFF;
	a.current.spk_mode = SPK_OFF;
	a.slot[3] = a.current;
	a.slot_valid = 0x9;
	a.led_mode = 1;
	a.charge_off = 1;

	n = persist_encode(&a, buf, sizeof(buf));
	CHECK(n > 0);
	CHECK(persist_encode(&a, buf, n - 1) == 0);

	memset(&b, 0, sizeof(b));
	CHECK(persist_decode(&b, buf, n));
	CHECK(memcmp(&a, &b, sizeof(a)) == 0);

	/* Any corruption is rejected and leaves the output untouched */
	for (size_t i = 0; i < n; i++) {
		struct persist c;

		memset(&c, 0xaa, sizeof(c));
		buf[i] ^= 0x10;
		CHECK(!persist_decode(&c, buf, n));
		CHECK(((uint8_t *)&c)[0] == 0xaa);
		buf[i] ^= 0x10;
	}
	CHECK(!persist_decode(&b, buf, n - 1));

	/* Out of range values are clamped */
	struct scene s;

	scene_default(&s);
	s.vol[0] = 5000;
	s.bal[0] = -128;
	s.master_db = 20;
	s.mute = 0xff;
	s.spk_mode = 9;
	scene_sanitize(&s);
	CHECK(s.vol[0] == 1023 && s.bal[0] == -BALANCE_MAX);
	CHECK(s.master_db == MASTER_DB_MAX && s.mute == 0x0f && s.spk_mode == SPK_AUTO);

	CHECK(master_db_to_gain(MASTER_DB_OFF) == 0.0f);
	CHECK_NEAR(master_db_to_gain(-6), 0.5012, 1e-3);
}
