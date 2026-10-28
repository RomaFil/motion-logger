/**
 * @file test_motion_report.c
 * @brief Перевірки звіту руху по секундах на вигаданому потоці відліків.
 *
 * Запуск: make test (звичайний) або make test-tsan. Код виходу 0 означає, що всі
 * перевірки пройшли. Заліза не потрібно: "сигнал" обчислюється формулами.
 */
#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "accel_csv.h"
#include "check.h"
#include "motion_report.h"

#define WINDOW 10
#define THRESHOLD 0.05
#define CONFIRM 20
#define SAMPLE_RATE_HZ 100
#define MSEC_PER_SAMPLE 10
#define PI 3.14159265358979323846

/* Вигаданий запис: 5 с стою, 5 с іду, 5 с стою. */
#define STILL_SECONDS 5
#define WALK_SECONDS 5
#define TOTAL_SECONDS (STILL_SECONDS + WALK_SECONDS + STILL_SECONDS)
#define MAX_ROWS 32
#define MAX_EVENTS 8

/* Допустима різниця при порівнянні дробових чисел. */
#define TOLERANCE 0.000001

/* Скільки перевірок не пройшло. main повертає це як код виходу. */
static int failures;

struct recorded_event {
	enum motion_event event;
	int64_t time_ms;
};

/* Що звіт повернув за весь прогін. */
struct run_result {
	struct motion_report_row rows[MAX_ROWS];
	size_t row_count;
	struct recorded_event events[MAX_EVENTS];
	size_t event_count;
};

static bool nearly_equal(double a, double b) { return a - b < TOLERANCE && b - a < TOLERANCE; }

/* Відлік вигаданого запису в момент time_ms: стою (0, 0, 1 g) або іду (z коливається). */
static struct accel_sample make_sample(int64_t time_ms)
{
	struct accel_sample sample = {.time_ms = time_ms, .x_g = 0.0, .y_g = 0.0, .z_g = 1.0};
	double seconds = (double)time_ms / 1000.0;

	if (seconds >= STILL_SECONDS && seconds < STILL_SECONDS + WALK_SECONDS) {
		sample.z_g = 1.0 + 0.4 * sin(2.0 * PI * 2.0 * seconds);
	}

	return sample;
}

static void remember(struct run_result *result, const struct motion_report_update *update,
		     int64_t time_ms)
{
	if (update->row_ready && result->row_count < MAX_ROWS) {
		result->rows[result->row_count++] = update->row;
	}
	if (update->event != MOTION_EVENT_NONE && result->event_count < MAX_EVENTS) {
		result->events[result->event_count].event = update->event;
		result->events[result->event_count].time_ms = time_ms;
		result->event_count++;
	}
}

static void run_whole_recording(struct run_result *result)
{
	struct motion_report report;
	struct motion_report_update update;
	int64_t time_ms;

	memset(result, 0, sizeof(*result));
	CHECK(motion_report_init(&report, WINDOW, THRESHOLD, CONFIRM) == 0);

	for (time_ms = 0; time_ms < TOTAL_SECONDS * 1000; time_ms += MSEC_PER_SAMPLE) {
		struct accel_sample sample = make_sample(time_ms);

		CHECK(motion_report_add_sample(&report, &sample, &update) == 0);
		remember(result, &update, time_ms);
	}
	CHECK(motion_report_finish(&report, &update) == 0);
	remember(result, &update, time_ms);
}

static void test_init_rejects_bad_arguments(void)
{
	struct motion_report report;

	CHECK(motion_report_init(NULL, WINDOW, THRESHOLD, CONFIRM) == -EINVAL);
	CHECK(motion_report_init(&report, 0, THRESHOLD, CONFIRM) == -EINVAL);
	CHECK(motion_report_init(&report, WINDOW, -1.0, CONFIRM) == -EINVAL);
	CHECK(motion_report_init(&report, WINDOW, THRESHOLD, 0) == -EINVAL);
}

static void test_one_row_per_second_with_all_samples(void)
{
	struct run_result result;
	size_t i;

	run_whole_recording(&result);

	CHECK(result.row_count == TOTAL_SECONDS);
	for (i = 0; i < result.row_count; i++) {
		CHECK(result.rows[i].second == (int64_t)i);
		CHECK(result.rows[i].samples == SAMPLE_RATE_HZ);
	}
}

static void test_rows_match_what_was_happening(void)
{
	struct run_result result;
	size_t i;

	run_whole_recording(&result);

	for (i = 0; i < result.row_count; i++) {
		bool walking = i >= STILL_SECONDS && i < STILL_SECONDS + WALK_SECONDS;

		CHECK(result.rows[i].moving == walking);
	}
	/* Стоячи, відхилення нуль; ідучи, помітно більше за поріг. */
	CHECK(nearly_equal(result.rows[1].average_deviation_g, 0.0));
	CHECK(result.rows[7].average_deviation_g > THRESHOLD);
}

static void test_exactly_one_start_and_one_stop_with_confirmation_delay(void)
{
	struct run_result result;

	run_whole_recording(&result);

	CHECK(result.event_count == 2);
	CHECK(result.events[0].event == MOTION_EVENT_START);
	CHECK(result.events[1].event == MOTION_EVENT_STOP);
	/* Подія приходить не раніше початку руху і не пізніше, ніж за 0,6 с після нього. */
	CHECK(result.events[0].time_ms >= STILL_SECONDS * 1000);
	CHECK(result.events[0].time_ms < STILL_SECONDS * 1000 + 600);
	CHECK(result.events[1].time_ms >= (STILL_SECONDS + WALK_SECONDS) * 1000);
	CHECK(result.events[1].time_ms < (STILL_SECONDS + WALK_SECONDS) * 1000 + 600);
}

static void test_finish_closes_the_open_second_once(void)
{
	struct motion_report report;
	struct motion_report_update update;
	struct accel_sample sample = {.time_ms = 3500, .x_g = 0.0, .y_g = 0.0, .z_g = 1.0};

	CHECK(motion_report_init(&report, WINDOW, THRESHOLD, CONFIRM) == 0);

	CHECK(motion_report_finish(&report, &update) == 0);
	CHECK(!update.row_ready);

	CHECK(motion_report_add_sample(&report, &sample, &update) == 0);
	CHECK(!update.row_ready);
	CHECK(motion_report_finish(&report, &update) == 0);
	CHECK(update.row_ready && update.row.second == 3 && update.row.samples == 1);
	CHECK(update.event == MOTION_EVENT_NONE);

	CHECK(motion_report_finish(&report, &update) == 0);
	CHECK(!update.row_ready);
}

/* Секунди без відліків у звіті відсутні, нумерація показує, де була дірка. */
static void test_gap_in_time_skips_missing_seconds(void)
{
	struct motion_report report;
	struct motion_report_update update;
	struct accel_sample first = {.time_ms = 500, .z_g = 1.0};
	struct accel_sample second = {.time_ms = 4200, .z_g = 1.0};

	CHECK(motion_report_init(&report, WINDOW, THRESHOLD, CONFIRM) == 0);
	CHECK(motion_report_add_sample(&report, &first, &update) == 0);
	CHECK(motion_report_add_sample(&report, &second, &update) == 0);

	CHECK(update.row_ready && update.row.second == 0);
	CHECK(motion_report_finish(&report, &update) == 0);
	CHECK(update.row_ready && update.row.second == 4);
}

static void test_backwards_time_and_bad_values_are_rejected(void)
{
	struct motion_report report;
	struct motion_report_update update;
	struct accel_sample good = {.time_ms = 2000, .z_g = 1.0};
	struct accel_sample earlier = {.time_ms = 1999, .z_g = 1.0};
	struct accel_sample not_a_number = {.time_ms = 2010, .z_g = NAN};

	CHECK(motion_report_init(&report, WINDOW, THRESHOLD, CONFIRM) == 0);
	CHECK(motion_report_add_sample(&report, &good, &update) == 0);

	CHECK(motion_report_add_sample(&report, &earlier, &update) == -EINVAL);
	CHECK(motion_report_add_sample(&report, &not_a_number, &update) == -EINVAL);
	CHECK(motion_report_add_sample(NULL, &good, &update) == -EINVAL);
	CHECK(motion_report_add_sample(&report, NULL, &update) == -EINVAL);
	CHECK(motion_report_add_sample(&report, &good, NULL) == -EINVAL);

	/* Стан не зіпсовано: у відкритій секунді все ще один відлік. */
	CHECK(motion_report_finish(&report, &update) == 0);
	CHECK(update.row_ready && update.row.samples == 1);
}

/*
 * Вікно 1 відлік, поріг 0,25 g: відлік z = 1,5 g рухомий, z = 1,0 g спокійний. Рівно 50 з 100
 * відліків рухомі: "більш ніж половина" не виконується, тож секунда НЕ рухома.
 */
static void test_exactly_half_moving_samples_is_not_a_moving_second(void)
{
	struct motion_report report;
	struct motion_report_update update;
	int i;

	CHECK(motion_report_init(&report, 1, 0.25, 1) == 0);
	for (i = 0; i < SAMPLE_RATE_HZ; i++) {
		struct accel_sample sample = {.time_ms = i * MSEC_PER_SAMPLE,
					      .z_g = i < SAMPLE_RATE_HZ / 2 ? 1.5 : 1.0};

		CHECK(motion_report_add_sample(&report, &sample, &update) == 0);
	}
	CHECK(motion_report_finish(&report, &update) == 0);

	CHECK(update.row_ready);
	CHECK(update.row.moving_samples == SAMPLE_RATE_HZ / 2);
	CHECK(update.row.samples == SAMPLE_RATE_HZ);
	CHECK(!update.row.moving);
}

/* 100 відліків з відхиленням 0,25 g (z = 1,25): середнє секунди рівно 0,25, а не сума 25. */
static void test_second_average_is_a_mean_not_a_sum(void)
{
	struct motion_report report;
	struct motion_report_update update;
	int i;

	CHECK(motion_report_init(&report, 1, 0.1, 1) == 0);
	for (i = 0; i < SAMPLE_RATE_HZ; i++) {
		struct accel_sample sample = {.time_ms = i * MSEC_PER_SAMPLE, .z_g = 1.25};

		CHECK(motion_report_add_sample(&report, &sample, &update) == 0);
	}
	CHECK(motion_report_finish(&report, &update) == 0);

	CHECK(update.row_ready);
	CHECK(nearly_equal(update.row.average_deviation_g, 0.25));
	CHECK(update.row.moving);
}

int main(void)
{
	test_init_rejects_bad_arguments();
	test_one_row_per_second_with_all_samples();
	test_rows_match_what_was_happening();
	test_exactly_one_start_and_one_stop_with_confirmation_delay();
	test_finish_closes_the_open_second_once();
	test_gap_in_time_skips_missing_seconds();
	test_backwards_time_and_bad_values_are_rejected();
	test_exactly_half_moving_samples_is_not_a_moving_second();
	test_second_average_is_a_mean_not_a_sum();

	if (failures != 0) {
		printf("%d check(s) failed\n", failures);
		return 1;
	}
	printf("motion_report: all checks passed\n");

	return 0;
}
