/**
 * @file test_motion_filter.c
 * @brief Перевірки фільтра руху на вигаданих даних: стою, іду, межі, помилкові входи.
 *
 * Запуск: make test (звичайний) або make test-tsan. Код виходу 0 означає, що всі
 * перевірки пройшли. Заліза не потрібно: "сигнал" тут обчислюється формулами.
 */
#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#include "check.h"
#include "motion_filter.h"

/* Вікно з трекера: ~10 відліків (0,1 с при 100 відліках на секунду). */
#define WINDOW 10

/* Початковий поріг для тестів. Справжній поріг буде підібрано за записом з плати (крок 3.5). */
#define THRESHOLD 0.05

/* Допустима різниця при порівнянні дробових чисел. */
#define TOLERANCE 0.000001

#define SAMPLE_RATE_HZ 100
#define WALK_SECONDS 3
#define STEPS_PER_SECOND 2.0
#define WALK_AMPLITUDE_G 0.4
#define PI 3.14159265358979323846

/* Скільки перевірок не пройшло. main повертає це як код виходу. */
static int failures;

static bool nearly_equal(double a, double b) { return a - b < TOLERANCE && b - a < TOLERANCE; }

/* Готує фільтр з розміром вікна і порогом; помилка ініціалізації це збій тесту. */
static void setup_filter(struct motion_filter *filter, size_t window, double threshold)
{
	CHECK(motion_filter_init(filter, window, threshold) == 0);
}

/* Додає n однакових відліків. */
static void add_repeated(struct motion_filter *filter, size_t n, double x, double y, double z)
{
	size_t i;

	for (i = 0; i < n; i++) {
		CHECK(motion_filter_add_sample(filter, x, y, z) == 0);
	}
}

static void test_init_rejects_bad_arguments(void)
{
	struct motion_filter filter;

	CHECK(motion_filter_init(NULL, WINDOW, THRESHOLD) == -EINVAL);
	CHECK(motion_filter_init(&filter, 0, THRESHOLD) == -EINVAL);
	CHECK(motion_filter_init(&filter, MOTION_FILTER_MAX_WINDOW + 1, THRESHOLD) == -EINVAL);
	CHECK(motion_filter_init(&filter, WINDOW, -0.1) == -EINVAL);
	CHECK(motion_filter_init(&filter, WINDOW, NAN) == -EINVAL);
	CHECK(motion_filter_init(&filter, MOTION_FILTER_MAX_WINDOW, 0.0) == 0);
	CHECK(motion_filter_init(&filter, 1, THRESHOLD) == 0);
}

static void test_new_filter_reports_no_motion(void)
{
	struct motion_filter filter;

	setup_filter(&filter, WINDOW, THRESHOLD);

	CHECK(!motion_filter_is_moving(&filter));
	CHECK(nearly_equal(motion_filter_get_average(&filter), 0.0));
}

/* Плата лежить: довжина вектора 1 g, відхилення нуль. */
static void test_standing_flat_is_not_motion(void)
{
	struct motion_filter filter;

	setup_filter(&filter, WINDOW, THRESHOLD);
	add_repeated(&filter, 3 * WINDOW, 0.0, 0.0, 1.0);

	CHECK(!motion_filter_is_moving(&filter));
	CHECK(nearly_equal(motion_filter_get_average(&filter), 0.0));
}

/* Напрямок сили тяжіння не має значення: нахилена плата так само "стоїть". */
static void test_standing_tilted_is_not_motion(void)
{
	struct motion_filter filter;

	setup_filter(&filter, WINDOW, THRESHOLD);
	add_repeated(&filter, 3 * WINDOW, 0.6, 0.0, 0.8);

	CHECK(!motion_filter_is_moving(&filter));
	CHECK(nearly_equal(motion_filter_get_average(&filter), 0.0));
}

/* Шум датчика, дрібний порівняно з порогом, руху не дає. */
static void test_small_noise_is_not_motion(void)
{
	struct motion_filter filter;
	size_t i;

	setup_filter(&filter, WINDOW, THRESHOLD);
	for (i = 0; i < 5 * WINDOW; i++) {
		double noise = (i % 2 == 0) ? 0.01 : -0.01;

		CHECK(motion_filter_add_sample(&filter, 0.0, 0.0, 1.0 + noise) == 0);
	}

	CHECK(!motion_filter_is_moving(&filter));
	CHECK(nearly_equal(motion_filter_get_average(&filter), 0.01));
}

/* Ходьба: довжина вектора коливається 0,6..1,4 g, дві "кроки" на секунду. */
static void test_walking_is_motion_once_window_is_full(void)
{
	struct motion_filter filter;
	size_t total = WALK_SECONDS * SAMPLE_RATE_HZ;
	size_t i;

	setup_filter(&filter, WINDOW, THRESHOLD);
	for (i = 0; i < total; i++) {
		double time_s = (double)i / SAMPLE_RATE_HZ;
		double z = 1.0 + WALK_AMPLITUDE_G * sin(2.0 * PI * STEPS_PER_SECOND * time_s);

		CHECK(motion_filter_add_sample(&filter, 0.0, 0.0, z) == 0);
		if (i + 1 >= WINDOW) {
			CHECK(motion_filter_is_moving(&filter));
		}
	}
}

/* Поки вікно не повне, навіть різкий удар не оголошується рухом. */
static void test_motion_needs_a_full_window(void)
{
	struct motion_filter filter;
	size_t i;

	setup_filter(&filter, WINDOW, THRESHOLD);
	for (i = 0; i < WINDOW - 1; i++) {
		CHECK(motion_filter_add_sample(&filter, 0.0, 0.0, 3.0) == 0);
		CHECK(!motion_filter_is_moving(&filter));
	}
	CHECK(motion_filter_add_sample(&filter, 0.0, 0.0, 3.0) == 0);
	CHECK(motion_filter_is_moving(&filter));
}

/* Рух припиняється рівно тоді, коли останній "рухомий" відлік витіснено з вікна. */
static void test_motion_ends_when_window_is_flushed(void)
{
	struct motion_filter filter;
	size_t i;

	setup_filter(&filter, WINDOW, THRESHOLD);
	/* Відхилення 1,0 g: z = 2,0. */
	add_repeated(&filter, WINDOW, 0.0, 0.0, 2.0);
	CHECK(motion_filter_is_moving(&filter));

	/* Після 9 спокійних відліків у вікні ще один рухомий: середнє 0,1 g > 0,05. */
	for (i = 0; i < WINDOW - 1; i++) {
		CHECK(motion_filter_add_sample(&filter, 0.0, 0.0, 1.0) == 0);
		CHECK(motion_filter_is_moving(&filter));
	}
	CHECK(nearly_equal(motion_filter_get_average(&filter), 0.1));

	/* Десятий спокійний відлік витісняє останній рухомий. */
	CHECK(motion_filter_add_sample(&filter, 0.0, 0.0, 1.0) == 0);
	CHECK(!motion_filter_is_moving(&filter));
	CHECK(nearly_equal(motion_filter_get_average(&filter), 0.0));
}

/* Точні числа: вікно з 4 відліків, відхилення 0,5; 0,5; 0,2; 0 -> середнє 0,3. */
static void test_average_is_exact_and_window_slides(void)
{
	struct motion_filter filter;

	setup_filter(&filter, 4, THRESHOLD);
	CHECK(motion_filter_add_sample(&filter, 0.0, 0.0, 1.5) == 0);
	/* Вікно ще не повне: ділимо на кількість набраних відліків (1), а не на розмір вікна (4).
	 */
	CHECK(nearly_equal(motion_filter_get_average(&filter), 0.5));
	CHECK(motion_filter_add_sample(&filter, 0.0, 0.0, 0.5) == 0);
	CHECK(motion_filter_add_sample(&filter, 0.0, 0.0, 1.2) == 0);
	CHECK(motion_filter_add_sample(&filter, 0.0, 0.0, 1.0) == 0);
	CHECK(nearly_equal(motion_filter_get_average(&filter), 0.3));

	/* П'ятий відлік (відхилення 0) витісняє перший (0,5): (0,5 + 0,2 + 0 + 0) / 4. */
	CHECK(motion_filter_add_sample(&filter, 0.0, 0.0, 1.0) == 0);
	CHECK(nearly_equal(motion_filter_get_average(&filter), 0.175));
}

/* Поріг строгий: середнє, що ДОРІВНЮЄ порогу, руху ще не дає. */
static void test_average_equal_to_threshold_is_not_motion(void)
{
	struct motion_filter filter;

	/* 1,25 і 0,25 точно подаються в двійковій системі, тож рівність без похибки. */
	setup_filter(&filter, 1, 0.25);
	CHECK(motion_filter_add_sample(&filter, 0.0, 0.0, 1.25) == 0);
	CHECK(nearly_equal(motion_filter_get_average(&filter), 0.25));
	CHECK(!motion_filter_is_moving(&filter));

	CHECK(motion_filter_add_sample(&filter, 0.0, 0.0, 1.375) == 0);
	CHECK(motion_filter_is_moving(&filter));
}

/* Довжина 0,5 g відхиляється від сили тяжіння так само, як 1,5 g: важить лише відстань. */
static void test_deviation_is_symmetric_around_gravity(void)
{
	struct motion_filter below;
	struct motion_filter above;

	setup_filter(&below, 1, THRESHOLD);
	setup_filter(&above, 1, THRESHOLD);
	CHECK(motion_filter_add_sample(&below, 0.0, 0.0, 0.5) == 0);
	CHECK(motion_filter_add_sample(&above, 0.0, 0.0, 1.5) == 0);

	CHECK(nearly_equal(motion_filter_get_average(&below), motion_filter_get_average(&above)));
}

static void test_bad_samples_are_rejected_and_state_is_kept(void)
{
	struct motion_filter filter;
	double before;

	setup_filter(&filter, WINDOW, THRESHOLD);
	add_repeated(&filter, WINDOW, 0.0, 0.0, 1.2);
	before = motion_filter_get_average(&filter);

	CHECK(motion_filter_add_sample(NULL, 0.0, 0.0, 1.0) == -EINVAL);
	CHECK(motion_filter_add_sample(&filter, NAN, 0.0, 1.0) == -EINVAL);
	CHECK(motion_filter_add_sample(&filter, 0.0, INFINITY, 1.0) == -EINVAL);
	CHECK(motion_filter_add_sample(&filter, 0.0, 0.0, -INFINITY) == -EINVAL);

	CHECK(nearly_equal(motion_filter_get_average(&filter), before));
}

int main(void)
{
	test_init_rejects_bad_arguments();
	test_new_filter_reports_no_motion();
	test_standing_flat_is_not_motion();
	test_standing_tilted_is_not_motion();
	test_small_noise_is_not_motion();
	test_walking_is_motion_once_window_is_full();
	test_motion_needs_a_full_window();
	test_motion_ends_when_window_is_flushed();
	test_average_is_exact_and_window_slides();
	test_average_equal_to_threshold_is_not_motion();
	test_deviation_is_symmetric_around_gravity();
	test_bad_samples_are_rejected_and_state_is_kept();

	if (failures != 0) {
		printf("%d check(s) failed\n", failures);
		return 1;
	}
	printf("motion_filter: all checks passed\n");

	return 0;
}
