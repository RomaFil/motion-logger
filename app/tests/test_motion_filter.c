/**
 * @file test_motion_filter.c
 * @brief Перевірки фільтра руху на вигаданих даних: стою, іду, межі, помилкові входи.
 *
 * Запуск: make test (звичайний) або make test-tsan. Код виходу 0 означає, що всі
 * перевірки пройшли. Заліза не потрібно: "сигнал" тут обчислюється формулами.
 *
 * Фільтр міряє відхилення довжини вектора від його повільного середнього (див.
 * motion_filter.h), тому "рух" у тестах це ЗМІНИ довжини, а не стале значення. Перший відлік
 * задає середнє: його відхилення нуль, а наступний відлік порівнюється з цим середнім.
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

/* Початковий поріг для тестів. Справжній поріг підібрано за записом з плати (motion_defaults.h). */
#define THRESHOLD 0.05

/* Допустима різниця при порівнянні дробових чисел, коли середнє фільтра не зсувається. */
#define TOLERANCE 0.000001

/*
 * Допуск там, де середнє довжини вектора встигає зсунутись: за один відлік воно зсувається
 * на 1/300 різниці, тож за десятки відліків похибка не перевищує кількох тисячних g.
 */
#define DRIFT_TOLERANCE 0.005

#define SAMPLE_RATE_HZ 100
#define WALK_SECONDS 3
#define STEPS_PER_SECOND 2.0
#define WALK_AMPLITUDE_G 0.4
#define PI 3.14159265358979323846

/* Скільки перевірок не пройшло. main повертає це як код виходу. */
static int failures;

static bool nearly_equal(double a, double b) { return a - b < TOLERANCE && b - a < TOLERANCE; }

static bool close_to(double a, double b)
{
	return a - b < DRIFT_TOLERANCE && b - a < DRIFT_TOLERANCE;
}

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

/* Плата лежить: довжина вектора стала, відхилення нуль. */
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

/*
 * Похибка масштабу датчика: нерухомий MPU6050 на платі показував 0,942 g замість 1 g
 * (запис 01.11.2026). Фільтр, який міряв би відхилення від ідеальної 1 g, вважав би це
 * сталим "рухом" (0,058 g при порозі 0,05). Тут цей випадок закріплено тестом.
 */
static void test_sensor_gain_error_is_not_motion(void)
{
	struct motion_filter filter;
	size_t i;

	setup_filter(&filter, WINDOW, THRESHOLD);
	for (i = 0; i < 5 * WINDOW; i++) {
		double noise = (i % 2 == 0) ? 0.002 : -0.002;

		CHECK(motion_filter_add_sample(&filter, 0.0, 0.0, 0.942 + noise) == 0);
	}

	CHECK(!motion_filter_is_moving(&filter));
	CHECK(motion_filter_get_average(&filter) < 0.01);
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
	CHECK(close_to(motion_filter_get_average(&filter), 0.01));
}

/* Середнє довжини вектора пристосовується: нове стале значення з часом перестає бути рухом. */
static void test_baseline_follows_a_new_steady_level(void)
{
	struct motion_filter filter;

	setup_filter(&filter, WINDOW, THRESHOLD);
	add_repeated(&filter, WINDOW, 0.0, 0.0, 1.0);
	CHECK(!motion_filter_is_moving(&filter));

	/* Стрибок до 1,1 g: спершу відхилення 0,1 g > порога, тож є рух. */
	add_repeated(&filter, WINDOW, 0.0, 0.0, 1.1);
	CHECK(motion_filter_is_moving(&filter));

	/* Після кількох сталих інтервалів середнє дігнало 1,1 g, і руху нема. */
	add_repeated(&filter, 10 * MOTION_FILTER_BASELINE_SAMPLES, 0.0, 0.0, 1.1);
	CHECK(!motion_filter_is_moving(&filter));
	CHECK(motion_filter_get_average(&filter) < 0.001);
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
	/* Перший відлік задає середнє (1 g), решта дев'ять відхиляються від нього на 2 g. */
	CHECK(motion_filter_add_sample(&filter, 0.0, 0.0, 1.0) == 0);
	for (i = 0; i < WINDOW - 2; i++) {
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
	/* Спокій задає середнє 1 g, потім один відлік-удар: відхилення 1,0 g. */
	add_repeated(&filter, 3 * WINDOW, 0.0, 0.0, 1.0);
	CHECK(motion_filter_add_sample(&filter, 0.0, 0.0, 2.0) == 0);
	CHECK(motion_filter_is_moving(&filter));
	CHECK(close_to(motion_filter_get_average(&filter), 0.1));

	/* Ще 9 спокійних відліків: удар лишається у вікні, середнє ≈ 0,1 g > 0,05. */
	for (i = 0; i < WINDOW - 1; i++) {
		CHECK(motion_filter_add_sample(&filter, 0.0, 0.0, 1.0) == 0);
		CHECK(motion_filter_is_moving(&filter));
	}
	CHECK(close_to(motion_filter_get_average(&filter), 0.1));

	/* Десятий спокійний відлік витісняє удар: лишається лише дрібний дрейф середнього. */
	CHECK(motion_filter_add_sample(&filter, 0.0, 0.0, 1.0) == 0);
	CHECK(!motion_filter_is_moving(&filter));
	CHECK(close_to(motion_filter_get_average(&filter), 0.0));
}

/* Вікно з 4 відліків: удар 0,4 g видно, поки він у вікні, і зникає, коли його витіснено. */
static void test_average_is_exact_and_window_slides(void)
{
	struct motion_filter filter;

	setup_filter(&filter, 4, THRESHOLD);
	/* Перший відлік задає середнє: його відхилення 0, ділимо на кількість набраних (1). */
	CHECK(motion_filter_add_sample(&filter, 0.0, 0.0, 1.0) == 0);
	CHECK(nearly_equal(motion_filter_get_average(&filter), 0.0));
	/* Удар 1,4 g проти середнього 1,0 g: відхилення рівно 0,4, вікно має 2 відліки. */
	CHECK(motion_filter_add_sample(&filter, 0.0, 0.0, 1.4) == 0);
	CHECK(nearly_equal(motion_filter_get_average(&filter), 0.2));
	CHECK(motion_filter_add_sample(&filter, 0.0, 0.0, 1.0) == 0);
	CHECK(motion_filter_add_sample(&filter, 0.0, 0.0, 1.0) == 0);
	CHECK(close_to(motion_filter_get_average(&filter), 0.1));

	/* П'ятий відлік витісняє перший (0), удар ще у вікні. */
	CHECK(motion_filter_add_sample(&filter, 0.0, 0.0, 1.0) == 0);
	CHECK(close_to(motion_filter_get_average(&filter), 0.1));

	/* Шостий витісняє удар: середнє близьке до нуля. */
	CHECK(motion_filter_add_sample(&filter, 0.0, 0.0, 1.0) == 0);
	CHECK(close_to(motion_filter_get_average(&filter), 0.0));
}

/* Поріг строгий: середнє, що ДОРІВНЮЄ порогу, руху ще не дає. */
static void test_average_equal_to_threshold_is_not_motion(void)
{
	struct motion_filter filter;

	/* 1,25 і 0,25 точно подаються в двійковій системі, тож рівність без похибки. */
	setup_filter(&filter, 1, 0.25);
	CHECK(motion_filter_add_sample(&filter, 0.0, 0.0, 1.0) == 0);
	CHECK(motion_filter_add_sample(&filter, 0.0, 0.0, 1.25) == 0);
	CHECK(nearly_equal(motion_filter_get_average(&filter), 0.25));
	CHECK(!motion_filter_is_moving(&filter));

	/* Наступний відлік 1,5 g далі від середнього (≈1,0008 g), ніж поріг. */
	CHECK(motion_filter_add_sample(&filter, 0.0, 0.0, 1.5) == 0);
	CHECK(motion_filter_is_moving(&filter));
}

/* Довжина 0,5 g відхиляється від середнього так само, як 1,5 g: важить лише відстань. */
static void test_deviation_is_symmetric_around_the_baseline(void)
{
	struct motion_filter below;
	struct motion_filter above;

	setup_filter(&below, 1, THRESHOLD);
	setup_filter(&above, 1, THRESHOLD);
	CHECK(motion_filter_add_sample(&below, 0.0, 0.0, 1.0) == 0);
	CHECK(motion_filter_add_sample(&above, 0.0, 0.0, 1.0) == 0);
	CHECK(motion_filter_add_sample(&below, 0.0, 0.0, 0.5) == 0);
	CHECK(motion_filter_add_sample(&above, 0.0, 0.0, 1.5) == 0);

	CHECK(nearly_equal(motion_filter_get_average(&below), 0.5));
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

/* Поріг виходу: від нуля до порога входу; усе інше, і NULL, це помилка. */
static void test_set_release_rejects_bad_arguments(void)
{
	struct motion_filter filter;

	setup_filter(&filter, 1, 0.25);
	CHECK(motion_filter_set_release(NULL, 0.1) == -EINVAL);
	CHECK(motion_filter_set_release(&filter, -0.1) == -EINVAL);
	CHECK(motion_filter_set_release(&filter, NAN) == -EINVAL);
	CHECK(motion_filter_set_release(&filter, 0.26) == -EINVAL);
	CHECK(motion_filter_set_release(&filter, 0.25) == 0);
	CHECK(motion_filter_set_release(&filter, 0.0) == 0);
}

/*
 * Гістерезис (вікно 1, вхід 0,25 g, вихід 0,1 g): відхилення між порогами не вмикає рух, але й не
 * вимикає його. Відлік «z = 1,0 + d» дає відхилення ≈ d, бо середнє зсувається на 1/300 за відлік.
 */
static void test_hysteresis_keeps_motion_between_the_thresholds(void)
{
	struct motion_filter filter;

	setup_filter(&filter, 1, 0.25);
	CHECK(motion_filter_set_release(&filter, 0.1) == 0);
	CHECK(motion_filter_add_sample(&filter, 0.0, 0.0, 1.0) == 0);

	/* 0,2 g: нижче порога входу, руху ще нема */
	CHECK(motion_filter_add_sample(&filter, 0.0, 0.0, 1.2) == 0);
	CHECK(!motion_filter_is_moving(&filter));

	/* 0,3 g: вище порога входу, рух почався */
	CHECK(motion_filter_add_sample(&filter, 0.0, 0.0, 1.3) == 0);
	CHECK(motion_filter_is_moving(&filter));

	/* ≈0,2 g: між порогами, рух триває (з одним порогом він би вже обірвався) */
	CHECK(motion_filter_add_sample(&filter, 0.0, 0.0, 1.2) == 0);
	CHECK(motion_filter_is_moving(&filter));

	/* ≈0,05 g: нижче порога виходу, рух закінчився */
	CHECK(motion_filter_add_sample(&filter, 0.0, 0.0, 1.05) == 0);
	CHECK(!motion_filter_is_moving(&filter));

	/* ≈0,2 g: між порогами, і рух знову не починається */
	CHECK(motion_filter_add_sample(&filter, 0.0, 0.0, 1.2) == 0);
	CHECK(!motion_filter_is_moving(&filter));
}

int main(void)
{
	test_set_release_rejects_bad_arguments();
	test_hysteresis_keeps_motion_between_the_thresholds();
	test_init_rejects_bad_arguments();
	test_new_filter_reports_no_motion();
	test_standing_flat_is_not_motion();
	test_standing_tilted_is_not_motion();
	test_sensor_gain_error_is_not_motion();
	test_small_noise_is_not_motion();
	test_baseline_follows_a_new_steady_level();
	test_walking_is_motion_once_window_is_full();
	test_motion_needs_a_full_window();
	test_motion_ends_when_window_is_flushed();
	test_average_is_exact_and_window_slides();
	test_average_equal_to_threshold_is_not_motion();
	test_deviation_is_symmetric_around_the_baseline();
	test_bad_samples_are_rejected_and_state_is_kept();

	if (failures != 0) {
		printf("%d check(s) failed\n", failures);
		return 1;
	}
	printf("motion_filter: all checks passed\n");

	return 0;
}
