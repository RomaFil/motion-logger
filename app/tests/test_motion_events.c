/**
 * @file test_motion_events.c
 * @brief Перевірки відстеження подій руху (підтвердження зміни стану).
 *
 * Запуск: make test (звичайний) або make test-tsan. Код виходу 0 означає, що всі
 * перевірки пройшли. Заліза не потрібно.
 */
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>

#include "check.h"
#include "motion_events.h"

#define CONFIRM 3

/* Скільки перевірок не пройшло. main повертає це як код виходу. */
static int failures;

static void test_init_rejects_bad_arguments(void)
{
	struct motion_events events;

	CHECK(motion_events_init(NULL, CONFIRM) == -EINVAL);
	CHECK(motion_events_init(&events, 0) == -EINVAL);
	CHECK(motion_events_init(&events, 1) == 0);
}

static void test_no_event_while_state_is_unchanged(void)
{
	struct motion_events events;
	int i;

	CHECK(motion_events_init(&events, CONFIRM) == 0);
	for (i = 0; i < 100; i++) {
		CHECK(motion_events_update(&events, false) == MOTION_EVENT_NONE);
	}
}

static void test_start_needs_confirm_samples_in_a_row(void)
{
	struct motion_events events;

	CHECK(motion_events_init(&events, CONFIRM) == 0);

	CHECK(motion_events_update(&events, true) == MOTION_EVENT_NONE);
	CHECK(motion_events_update(&events, true) == MOTION_EVENT_NONE);
	CHECK(motion_events_update(&events, true) == MOTION_EVENT_START);
	/* Стан вже "рухається": далі тих самих відповідей подій нема. */
	CHECK(motion_events_update(&events, true) == MOTION_EVENT_NONE);
}

static void test_stop_needs_confirm_samples_in_a_row(void)
{
	struct motion_events events;
	int i;

	CHECK(motion_events_init(&events, CONFIRM) == 0);
	for (i = 0; i < CONFIRM; i++) {
		(void)motion_events_update(&events, true);
	}

	CHECK(motion_events_update(&events, false) == MOTION_EVENT_NONE);
	CHECK(motion_events_update(&events, false) == MOTION_EVENT_NONE);
	CHECK(motion_events_update(&events, false) == MOTION_EVENT_STOP);
	CHECK(motion_events_update(&events, false) == MOTION_EVENT_NONE);
}

/* Одиночний збій у відповідях скидає підтвердження: події нема, брязкіт приглушено. */
static void test_glitch_resets_the_streak(void)
{
	struct motion_events events;

	CHECK(motion_events_init(&events, CONFIRM) == 0);

	CHECK(motion_events_update(&events, true) == MOTION_EVENT_NONE);
	CHECK(motion_events_update(&events, true) == MOTION_EVENT_NONE);
	CHECK(motion_events_update(&events, false) == MOTION_EVENT_NONE); /* збій */
	CHECK(motion_events_update(&events, true) == MOTION_EVENT_NONE);
	CHECK(motion_events_update(&events, true) == MOTION_EVENT_NONE);
	/* Лише тепер третя підряд відповідь "рухається". */
	CHECK(motion_events_update(&events, true) == MOTION_EVENT_START);
}

/* Підтвердження 1 це події без затримки: кожна зміна відповіді одразу подія. */
static void test_confirm_of_one_reacts_immediately(void)
{
	struct motion_events events;

	CHECK(motion_events_init(&events, 1) == 0);

	CHECK(motion_events_update(&events, true) == MOTION_EVENT_START);
	CHECK(motion_events_update(&events, false) == MOTION_EVENT_STOP);
	CHECK(motion_events_update(&events, true) == MOTION_EVENT_START);
}

/* Повний цикл: старт, стоп, знову старт; кожна подія рівно один раз. */
static void test_full_cycle_produces_one_event_per_change(void)
{
	struct motion_events events;
	int starts = 0;
	int stops = 0;
	int i;

	CHECK(motion_events_init(&events, CONFIRM) == 0);
	for (i = 0; i < 30; i++) {
		bool moving = (i >= 5 && i < 15) || i >= 22;
		enum motion_event event = motion_events_update(&events, moving);

		starts += event == MOTION_EVENT_START;
		stops += event == MOTION_EVENT_STOP;
	}

	CHECK(starts == 2);
	CHECK(stops == 1);
}

int main(void)
{
	test_init_rejects_bad_arguments();
	test_no_event_while_state_is_unchanged();
	test_start_needs_confirm_samples_in_a_row();
	test_stop_needs_confirm_samples_in_a_row();
	test_glitch_resets_the_streak();
	test_confirm_of_one_reacts_immediately();
	test_full_cycle_produces_one_event_per_change();

	if (failures != 0) {
		printf("%d check(s) failed\n", failures);
		return 1;
	}
	printf("motion_events: all checks passed\n");

	return 0;
}
