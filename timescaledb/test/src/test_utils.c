/*
 * This file and its contents are licensed under the Apache License 2.0.
 * Please see the included NOTICE for copyright information and
 * LICENSE-APACHE for a copy of the license.
 */
#include <postgres.h>

#include "test_utils.h"

/*
 * Test assertion macros.
 *
 * Errors are expected since we want to test that the macros work. For each
 * macro, test one failing and one non-failing condition. The non-failing must
 * come first since the failing one will abort the function.
 */
TS_TEST_FN(ts_test_utils_condition)
{
	bool true_value = true;
	bool false_value = false;

	TestAssertTrue(true_value == true_value);
	TestAssertTrue(true_value == false_value);

	PG_RETURN_VOID();
}

TS_TEST_FN(ts_test_utils_int64_eq)
{
	int64 big = 32532978;
	int64 small = 3242234;

	TestAssertInt64Eq(big, small);
	TestAssertInt64Eq(big, big);

	PG_RETURN_VOID();
}

TS_TEST_FN(ts_test_utils_ptr_eq)
{
	bool true_value = true;
	bool false_value = false;
	bool *true_ptr = &true_value;
	bool *false_ptr = &false_value;

	TestAssertPtrEq(true_ptr, true_ptr);
	TestAssertPtrEq(true_ptr, false_ptr);

	PG_RETURN_VOID();
}

TS_TEST_FN(ts_test_utils_double_eq)
{
	double big_double = 923423478.3242;
	double small_double = 324.3;

	TestAssertDoubleEq(big_double, big_double);
	TestAssertDoubleEq(big_double, small_double);

	PG_RETURN_VOID();
}
