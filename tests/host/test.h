/*
 * SPDX-License-Identifier: MIT
 *
 * Minimal test harness.
 */

#ifndef SP1M_TEST_H_
#define SP1M_TEST_H_

#include <math.h>
#include <stdio.h>

extern int test_failures;
extern int test_checks;

#define CHECK(cond)							\
	do {								\
		test_checks++;						\
		if (!(cond)) {						\
			test_failures++;				\
			printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
		}							\
	} while (0)

#define CHECK_NEAR(a, b, tol)						\
	do {								\
		double a_ = (double)(a), b_ = (double)(b);		\
		test_checks++;						\
		if (!(fabs(a_ - b_) <= (double)(tol))) {		\
			test_failures++;				\
			printf("  FAIL %s:%d: %s = %g, expected %g +- %g\n", \
			       __FILE__, __LINE__, #a, a_, b_, (double)(tol)); \
		}							\
	} while (0)

#define TEST(name) void name(void)

#endif /* SP1M_TEST_H_ */
