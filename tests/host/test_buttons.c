/*
 * SPDX-License-Identifier: MIT
 */

#include "buttons.h"
#include "test.h"

#define T(n) BTN_BIT(BTN_T##n)

TEST(test_buttons_decode)
{
	CHECK(ladder1_decode(0) == 0);
	CHECK(ladder1_decode(-5) == 0);
	CHECK(ladder1_decode(213) == T(1));
	CHECK(ladder1_decode(403) == T(2));
	CHECK(ladder1_decode(733) == T(3));
	CHECK(ladder1_decode(1220) == T(4));
	CHECK(ladder1_decode(1303) == (T(1) | T(4)));
	CHECK(ladder1_decode(1743) == BTN_TRACK_MASK);
	CHECK(ladder1_decode(1810) == BTN_BIT(BTN_PLAY));
	CHECK(ladder1_decode(2159) == (BTN_BIT(BTN_PLAY) | T(4)));
	CHECK(ladder1_decode(3000) == BTN_INVALID);

	CHECK(ladder2_decode(100) == 0);
	CHECK(ladder2_decode(404) == BTN_BIT(BTN_BACK));
	CHECK(ladder2_decode(729) == BTN_BIT(BTN_VOL_DOWN));
	CHECK(ladder2_decode(1220) == BTN_BIT(BTN_FWD));
	CHECK(ladder2_decode(1820) == BTN_BIT(BTN_VOL_UP));
	CHECK(ladder2_decode(1998) == (BTN_BIT(BTN_VOL_UP) | BTN_BIT(BTN_VOL_DOWN)));

	CHECK(buttons_from_ladders(213, 729) == (T(1) | BTN_BIT(BTN_VOL_DOWN)));
	/* Sagged PLAY while a volume button is down */
	CHECK(buttons_from_ladders(1740, 1820) ==
	      (BTN_BIT(BTN_PLAY) | BTN_BIT(BTN_VOL_UP)));
	/* Without ladder 2 the same level is all four tracks */
	CHECK(buttons_from_ladders(1740, 0) == BTN_TRACK_MASK);
	CHECK(buttons_from_ladders(4000, 0) == BTN_INVALID);
}

TEST(test_buttons_debounce)
{
	struct btn_debounce d = { 0 };

	CHECK(btn_debounce_update(&d, T(1), 2) == 0);
	CHECK(btn_debounce_update(&d, T(1), 2) == T(1));
	/* A single transient sample does not change the state */
	CHECK(btn_debounce_update(&d, T(2), 2) == T(1));
	CHECK(btn_debounce_update(&d, T(1), 2) == T(1));
	/* Invalid levels never become stable */
	CHECK(btn_debounce_update(&d, BTN_INVALID, 2) == T(1));
	CHECK(btn_debounce_update(&d, BTN_INVALID, 2) == T(1));
	CHECK(btn_debounce_update(&d, 0, 2) == T(1));
	CHECK(btn_debounce_update(&d, 0, 2) == 0);
}
