/**
 * @file test_accel_csv.c
 * @brief Перевірки розбору рядків CSV-запису акселерометра.
 *
 * Запуск: make test (звичайний) або make test-tsan. Код виходу 0 означає, що всі
 * перевірки пройшли. Заліза не потрібно.
 */
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "accel_csv.h"
#include "check.h"

/* Допустима різниця при порівнянні дробових чисел. */
#define TOLERANCE 0.000001

/* Скільки перевірок не пройшло. main повертає це як код виходу. */
static int failures;

static bool nearly_equal(double a, double b) { return a - b < TOLERANCE && b - a < TOLERANCE; }

static void test_valid_line_is_parsed(void)
{
	struct accel_sample sample;

	CHECK(accel_csv_parse_line("20460,0.1772,-0.0922,0.7738", &sample) == 0);
	CHECK(sample.time_ms == 20460);
	CHECK(nearly_equal(sample.x_g, 0.1772));
	CHECK(nearly_equal(sample.y_g, -0.0922));
	CHECK(nearly_equal(sample.z_g, 0.7738));
}

static void test_line_endings_are_ignored(void)
{
	struct accel_sample sample;

	CHECK(accel_csv_parse_line("0,0,0,1\n", &sample) == 0);
	CHECK(sample.time_ms == 0 && nearly_equal(sample.z_g, 1.0));
	CHECK(accel_csv_parse_line("10,0.5,-0.5,1\r\n", &sample) == 0);
	CHECK(sample.time_ms == 10 && nearly_equal(sample.x_g, 0.5));
}

static void test_integers_and_negative_values_are_accepted(void)
{
	struct accel_sample sample;

	CHECK(accel_csv_parse_line("5,-1,2,-3", &sample) == 0);
	CHECK(nearly_equal(sample.x_g, -1.0));
	CHECK(nearly_equal(sample.y_g, 2.0));
	CHECK(nearly_equal(sample.z_g, -3.0));
}

static void test_comments_and_blank_lines_are_skipped(void)
{
	struct accel_sample sample;

	CHECK(accel_csv_parse_line("# t_ms,x_g,y_g,z_g", &sample) == -ENODATA);
	CHECK(accel_csv_parse_line("#", &sample) == -ENODATA);
	CHECK(accel_csv_parse_line("", &sample) == -ENODATA);
	CHECK(accel_csv_parse_line("\n", &sample) == -ENODATA);
	CHECK(accel_csv_parse_line("\r\n", &sample) == -ENODATA);
}

static void test_wrong_number_of_fields_is_rejected(void)
{
	struct accel_sample sample;

	CHECK(accel_csv_parse_line("1", &sample) == -EINVAL);
	CHECK(accel_csv_parse_line("1,2,3", &sample) == -EINVAL);
	CHECK(accel_csv_parse_line("1,2,3,4,5", &sample) == -EINVAL);
	CHECK(accel_csv_parse_line("1,2,3,", &sample) == -EINVAL);
	CHECK(accel_csv_parse_line(",1,2,3", &sample) == -EINVAL);
}

static void test_bad_numbers_are_rejected(void)
{
	struct accel_sample sample;

	CHECK(accel_csv_parse_line("-1,0,0,1", &sample) == -EINVAL);
	CHECK(accel_csv_parse_line("1.5,0,0,1", &sample) == -EINVAL);
	CHECK(accel_csv_parse_line("abc,0,0,1", &sample) == -EINVAL);
	CHECK(accel_csv_parse_line("1,nan,0,1", &sample) == -EINVAL);
	CHECK(accel_csv_parse_line("1,0,inf,1", &sample) == -EINVAL);
	CHECK(accel_csv_parse_line("1,0,0,1e3", &sample) == -EINVAL);
	CHECK(accel_csv_parse_line("1,0x10,0,1", &sample) == -EINVAL);
	CHECK(accel_csv_parse_line("1, 0,0,1", &sample) == -EINVAL);
	CHECK(accel_csv_parse_line("1,+0.5,0,1", &sample) == -EINVAL);
	CHECK(accel_csv_parse_line("1,0.,0,1", &sample) == -EINVAL);
	CHECK(accel_csv_parse_line("1,.5,0,1", &sample) == -EINVAL);
	CHECK(accel_csv_parse_line("1,-,0,1", &sample) == -EINVAL);
	CHECK(accel_csv_parse_line("1,0,0,1x", &sample) == -EINVAL);
}

static void test_time_limits(void)
{
	struct accel_sample sample;

	CHECK(accel_csv_parse_line("999999999999999,0,0,1", &sample) == 0);
	CHECK(sample.time_ms == 999999999999999LL);
	CHECK(accel_csv_parse_line("1000000000000000,0,0,1", &sample) == -EINVAL);
}

static void test_too_long_line_is_rejected(void)
{
	char line[ACCEL_CSV_MAX_LINE + 20];
	struct accel_sample sample;

	memset(line, '1', sizeof(line) - 1);
	line[sizeof(line) - 1] = '\0';

	CHECK(accel_csv_parse_line(line, &sample) == -EINVAL);
}

static void test_failed_parse_leaves_result_untouched(void)
{
	struct accel_sample sample;
	struct accel_sample before;

	memset(&sample, 0x5A, sizeof(sample));
	before = sample;

	CHECK(accel_csv_parse_line("1,2,3,abc", &sample) == -EINVAL);
	CHECK(memcmp(&sample, &before, sizeof(sample)) == 0);
	CHECK(accel_csv_parse_line("# comment", &sample) == -ENODATA);
	CHECK(memcmp(&sample, &before, sizeof(sample)) == 0);
}

static void test_null_arguments_are_rejected(void)
{
	struct accel_sample sample;

	CHECK(accel_csv_parse_line(NULL, &sample) == -EINVAL);
	CHECK(accel_csv_parse_line("1,2,3,4", NULL) == -EINVAL);
}

int main(void)
{
	test_valid_line_is_parsed();
	test_line_endings_are_ignored();
	test_integers_and_negative_values_are_accepted();
	test_comments_and_blank_lines_are_skipped();
	test_wrong_number_of_fields_is_rejected();
	test_bad_numbers_are_rejected();
	test_time_limits();
	test_too_long_line_is_rejected();
	test_failed_parse_leaves_result_untouched();
	test_null_arguments_are_rejected();

	if (failures != 0) {
		printf("%d check(s) failed\n", failures);
		return 1;
	}
	printf("accel_csv: all checks passed\n");

	return 0;
}
