/*
 * SPDX-License-Identifier: MIT
 */

#include "fader.h"
#include "test.h"

TEST(test_fader_filter)
{
	struct fader f;
	uint16_t v;

	fader_init(&f);
	CHECK(fader_update(&f, 0, 0.2f) == 0);
	CHECK(fader_update(&f, FADER_RAW_HI + 100, 0.2f) == 0); /* median rejects */
	for (int i = 0; i < 100; i++) {
		v = fader_update(&f, 5000, 0.2f);
	}
	CHECK(v == FADER_MAX);

	/* Small noise around a position does not move the output */
	for (int i = 0; i < 100; i++) {
		v = fader_update(&f, 1850, 0.2f);
	}
	uint16_t settled = v;

	for (int i = 0; i < 200; i++) {
		v = fader_update(&f, 1850 + ((i & 1) ? 6 : -6), 0.2f);
		CHECK(v == settled);
	}
	CHECK_NEAR(settled, (1850.0 - FADER_RAW_LO) * FADER_MAX /
			    (FADER_RAW_HI - FADER_RAW_LO), 3.5);
}

TEST(test_fader_taper_and_pickup)
{
	struct pickup p;
	float prev = -1.0f;

	CHECK(fader_to_gain(0) == 0.0f);
	CHECK(fader_to_gain(FADER_MAX / 50) == 0.0f);
	CHECK_NEAR(fader_to_gain(FADER_MAX), 1.0, 1e-6);
	for (int i = 0; i <= FADER_MAX; i++) {
		float g = fader_to_gain((uint16_t)i);

		CHECK(g >= prev);
		prev = g;
	}

	/* Fader below target: engages when it crosses upwards */
	pickup_arm(&p, 600, 200);
	CHECK(p.pending);
	CHECK(!pickup_update(&p, 400));
	CHECK(!pickup_update(&p, 550));
	CHECK(pickup_update(&p, 700));	/* jumped across */
	CHECK(pickup_update(&p, 100));	/* stays engaged */

	/* Reaching the tolerance window engages too */
	pickup_arm(&p, 600, 900);
	CHECK(!pickup_update(&p, 700));
	CHECK(pickup_update(&p, 610));

	/* Below the stored value, pulling down takes over at once */
	pickup_arm(&p, 1023, 0);
	CHECK(!pickup_update(&p, 300));
	CHECK(!pickup_update(&p, 290));		/* jitter is not a pull */
	CHECK(pickup_update(&p, 250));
	pickup_arm(&p, 800, 400);
	CHECK(pickup_update(&p, 370));

	/* Already in place: no pickup needed */
	pickup_arm(&p, 600, 605);
	CHECK(!p.pending);
}
