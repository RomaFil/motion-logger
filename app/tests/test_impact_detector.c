/**
 * @file test_impact_detector.c
 * @brief Перевірки виявлення вільного падіння й удару (core/impact_detector).
 *
 * Відліки подаються з частотою 100 Гц (крок 10 мс), як це робить драйвер IIO. Запуск: make test.
 */
#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>

#include "check.h"
#include "impact_detector.h"

#define STEP_MS 10

/* Скільки перевірок не пройшло. main повертає це як код виходу. */
static int failures;

/* Подає count відліків із постійним модулем magnitude_g по осі Z, починаючи з *time_ms.
 * Повертає, скільки подій kind видано. */
static int feed(struct impact_detector *detector, int64_t *time_ms, int count, double magnitude_g,
		enum impact_event_kind kind, struct impact_event *last)
{
	int events = 0;
	int i;

	for (i = 0; i < count; i++) {
		struct impact_event event;

		CHECK(impact_detector_add_sample(detector, *time_ms, 0.0, 0.0, magnitude_g,
						 &event) == 0);
		if (event.kind == kind) {
			events++;
			if (last != NULL) {
				*last = event;
			}
		} else {
			CHECK(event.kind == IMPACT_EVENT_NONE);
		}
		*time_ms += STEP_MS;
	}

	return events;
}

static void test_rest_gives_no_events(void)
{
	struct impact_detector detector;
	int64_t time_ms = 0;

	impact_detector_init(&detector);
	CHECK(feed(&detector, &time_ms, 1000, 1.0, IMPACT_EVENT_NONE, NULL) == 1000);
}

/* Невагомість 300 мс: рівно одна подія, на відліку, де досягнуто 100 мс, з правильною тривалістю.
 */
static void test_free_fall_is_reported_once_after_minimum_duration(void)
{
	struct impact_detector detector;
	struct impact_event event = {0};
	int64_t time_ms = 0;

	impact_detector_init(&detector);
	CHECK(feed(&detector, &time_ms, 50, 1.0, IMPACT_EVENT_FREE_FALL, NULL) == 0);
	/* 30 відліків по 10 мс = 300 мс невагомості (від 500 до 790 мс). */
	CHECK(feed(&detector, &time_ms, 30, 0.05, IMPACT_EVENT_FREE_FALL, &event) == 1);
	CHECK(event.kind == IMPACT_EVENT_FREE_FALL);
	CHECK(event.duration_ms == IMPACT_DETECTOR_FREE_FALL_MIN_MS);
	CHECK(fabs(event.magnitude_g - 0.05) < 1e-9);
}

/* Дві окремі падіння дають дві події: епізод закінчується, коли |a| вище порога виходу. */
static void test_second_free_fall_after_recovery_is_reported(void)
{
	struct impact_detector detector;
	int64_t time_ms = 0;

	impact_detector_init(&detector);
	CHECK(feed(&detector, &time_ms, 30, 0.0, IMPACT_EVENT_FREE_FALL, NULL) == 1);
	CHECK(feed(&detector, &time_ms, 10, 1.0, IMPACT_EVENT_FREE_FALL, NULL) == 0);
	CHECK(feed(&detector, &time_ms, 30, 0.0, IMPACT_EVENT_FREE_FALL, NULL) == 1);
}

/* Коротка невагомість (50 мс) це збій чи підскок датчика, а не падіння. */
static void test_short_weightlessness_is_ignored(void)
{
	struct impact_detector detector;
	int64_t time_ms = 0;

	impact_detector_init(&detector);
	CHECK(feed(&detector, &time_ms, 5, 0.0, IMPACT_EVENT_FREE_FALL, NULL) == 0);
	CHECK(feed(&detector, &time_ms, 5, 1.0, IMPACT_EVENT_FREE_FALL, NULL) == 0);
	CHECK(feed(&detector, &time_ms, 5, 0.0, IMPACT_EVENT_FREE_FALL, NULL) == 0);
}

/*
 * Гістерезис: |a| коливається між 0,3 і 0,5 g. Значення 0,5 вище порога падіння (0,4), але нижче
 * порога виходу (0,6), тож епізод не переривається, а подія приходить один раз.
 */
static void test_hysteresis_keeps_the_episode_alive(void)
{
	struct impact_detector detector;
	int64_t time_ms = 0;
	int events = 0;
	int i;

	impact_detector_init(&detector);
	for (i = 0; i < 40; i++) {
		events += feed(&detector, &time_ms, 1, i % 2 == 0 ? 0.3 : 0.5,
			       IMPACT_EVENT_FREE_FALL, NULL);
	}
	CHECK(events == 1);
}

static void test_impact_is_reported_with_holdoff(void)
{
	struct impact_detector detector;
	struct impact_event event = {0};
	int64_t time_ms = 1000;

	impact_detector_init(&detector);
	CHECK(feed(&detector, &time_ms, 1, 3.0, IMPACT_EVENT_IMPACT, &event) == 1);
	CHECK(fabs(event.magnitude_g - 3.0) < 1e-9);
	CHECK(event.duration_ms == 0);

	/* Ще удари протягом 490 мс не рахуються. */
	CHECK(feed(&detector, &time_ms, 49, 3.2, IMPACT_EVENT_IMPACT, NULL) == 0);
	/* На 500-й мілісекунді після першого удару знову. */
	CHECK(feed(&detector, &time_ms, 1, 3.2, IMPACT_EVENT_IMPACT, NULL) == 1);
}

static void test_below_impact_threshold_is_ignored(void)
{
	struct impact_detector detector;
	int64_t time_ms = 0;

	impact_detector_init(&detector);
	CHECK(feed(&detector, &time_ms, 100, 2.4, IMPACT_EVENT_IMPACT, NULL) == 0);
}

/* Модуль вектора не залежить від орієнтації плати: удар по осі X теж удар. */
static void test_magnitude_ignores_orientation(void)
{
	struct impact_detector detector;
	struct impact_event event;

	impact_detector_init(&detector);
	CHECK(impact_detector_add_sample(&detector, 0, 1.8, 1.8, 0.5, &event) == 0);
	CHECK(event.kind == IMPACT_EVENT_IMPACT); /* sqrt(1,8² + 1,8² + 0,5²) = 2,6 g */
	CHECK(fabs(event.magnitude_g - sqrt(1.8 * 1.8 * 2 + 0.25)) < 1e-9);
}

/* Типовий випадок: падіння, потім удар об землю. */
static void test_fall_then_impact_gives_both_events_in_order(void)
{
	struct impact_detector detector;
	struct impact_event event;
	int64_t time_ms = 0;
	int free_fall_seen = 0;

	impact_detector_init(&detector);
	feed(&detector, &time_ms, 5, 1.0, IMPACT_EVENT_NONE, NULL);
	free_fall_seen = feed(&detector, &time_ms, 20, 0.0, IMPACT_EVENT_FREE_FALL, NULL);
	CHECK(free_fall_seen == 1);
	CHECK(impact_detector_add_sample(&detector, time_ms, 0.0, 0.0, 4.5, &event) == 0);
	CHECK(event.kind == IMPACT_EVENT_IMPACT);
	CHECK(fabs(event.magnitude_g - 4.5) < 1e-9);
}

static void test_bad_input_is_rejected(void)
{
	struct impact_detector detector;
	struct impact_event event;

	impact_detector_init(&detector);
	CHECK(impact_detector_add_sample(NULL, 0, 0, 0, 1, &event) == -EINVAL);
	CHECK(impact_detector_add_sample(&detector, 0, 0, 0, 1, NULL) == -EINVAL);
	CHECK(impact_detector_add_sample(&detector, 0, NAN, 0, 1, &event) == -EINVAL);
	CHECK(impact_detector_add_sample(&detector, 0, 0, INFINITY, 1, &event) == -EINVAL);

	CHECK(impact_detector_add_sample(&detector, 1000, 0, 0, 1, &event) == 0);
	CHECK(impact_detector_add_sample(&detector, 999, 0, 0, 1, &event) == -EINVAL);
	CHECK(impact_detector_add_sample(&detector, 1000, 0, 0, 1, &event) ==
	      0); /* той самий час можна */
}

int main(void)
{
	test_rest_gives_no_events();
	test_free_fall_is_reported_once_after_minimum_duration();
	test_second_free_fall_after_recovery_is_reported();
	test_short_weightlessness_is_ignored();
	test_hysteresis_keeps_the_episode_alive();
	test_impact_is_reported_with_holdoff();
	test_below_impact_threshold_is_ignored();
	test_magnitude_ignores_orientation();
	test_fall_then_impact_gives_both_events_in_order();
	test_bad_input_is_rejected();

	if (failures != 0) {
		printf("%d check(s) failed\n", failures);
		return 1;
	}
	printf("impact_detector: all checks passed\n");

	return 0;
}
