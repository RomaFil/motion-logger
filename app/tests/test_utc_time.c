/**
 * @file test_utc_time.c
 * @brief Перевірки перетворення дати UTC в секунди з 1970 року й назад (core/utc_time).
 *
 * Еталонні числа пораховано незалежно (Python, datetime). Запуск: make test.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "check.h"
#include "utc_time.h"

/* Скільки перевірок не пройшло. main повертає це як код виходу. */
static int failures;

struct known_time {
	int year;
	int month;
	int day;
	uint32_t seconds_of_day;
	int64_t epoch;
	const char *text;
};

static const struct known_time KNOWN[] = {
    {1970, 1, 1, 0, 0, "1970-01-01T00:00:00Z"},
    {2000, 2, 29, 86399, 951868799, "2000-02-29T23:59:59Z"},   /* високосний рік кратний 400 */
    {2024, 2, 29, 0, 1709164800, "2024-02-29T00:00:00Z"},      /* звичайний високосний */
    {2026, 3, 1, 0, 1772323200, "2026-03-01T00:00:00Z"},       /* 2026 не високосний */
    {2026, 10, 28, 43208, 1793188808, "2026-10-28T12:00:08Z"}, /* дата наших тестових даних */
    {2099, 12, 31, 86399, 4102444799LL, "2099-12-31T23:59:59Z"},
};

static void test_known_dates_convert_both_ways(void)
{
	size_t i;

	for (i = 0; i < sizeof(KNOWN) / sizeof(KNOWN[0]); i++) {
		const struct known_time *known = &KNOWN[i];
		char text[UTC_TIME_TEXT_SIZE];

		CHECK(utc_time_to_epoch(known->year, known->month, known->day,
					known->seconds_of_day) == known->epoch);
		CHECK(utc_time_format(text, sizeof(text), known->epoch) == 20);
		CHECK(strcmp(text, known->text) == 0);
	}
}

/* Кожна доба в діапазоні 2000..2099 дає свою епоху, без пропусків і повторів. */
static void test_every_day_in_a_century_round_trips(void)
{
	int64_t epoch;
	int64_t previous = -1;

	for (epoch = utc_time_to_epoch(2000, 1, 1, 0); epoch < utc_time_to_epoch(2100, 1, 1, 0);
	     epoch += 86400) {
		char text[UTC_TIME_TEXT_SIZE];
		int year;
		int month;
		int day;

		CHECK(utc_time_format(text, sizeof(text), epoch) == 20);
		CHECK(sscanf(text, "%d-%d-%dT", &year, &month, &day) == 3);
		CHECK(utc_time_to_epoch(year, month, day, 0) == epoch);
		CHECK(epoch > previous);
		previous = epoch;
	}
}

static void test_format_rejects_bad_input(void)
{
	char text[UTC_TIME_TEXT_SIZE];

	CHECK(utc_time_format(NULL, sizeof(text), 0) == -1);
	CHECK(utc_time_format(text, UTC_TIME_TEXT_SIZE - 1, 0) == -1);
	CHECK(utc_time_format(text, sizeof(text), -1) == -1);
	CHECK(utc_time_format(text, sizeof(text), 253402300800LL) == -1);
	CHECK(utc_time_format(text, sizeof(text), 253402300799LL) == 20);
}

int main(void)
{
	test_known_dates_convert_both_ways();
	test_every_day_in_a_century_round_trips();
	test_format_rejects_bad_input();

	if (failures != 0) {
		printf("%d check(s) failed\n", failures);
		return 1;
	}
	printf("utc_time: all checks passed\n");

	return 0;
}
