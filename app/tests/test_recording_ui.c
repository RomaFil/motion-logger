/**
 * @file test_recording_ui.c
 * @brief Перевірки логіки кнопки й етапів світлодіодів (core/recording_ui).
 *
 * Запуск: make test (звичайний) або make test-tsan. Код виходу 0 означає, що все пройшло.
 */
#include <stdio.h>

#include "check.h"
#include "recording_ui.h"

/* Скільки перевірок не пройшло. main повертає це як код виходу. */
static int failures;

static void test_initial_state_is_recording_and_still(void)
{
	struct recording_ui ui;

	recording_ui_init(&ui);
	CHECK(ui.recording);
	CHECK(recording_ui_view(&ui) == UI_VIEW_BEFORE);
}

/* Етапи за рухом: до руху, рух, після зупинки; повторний рух знову дає «рух». */
static void test_view_follows_motion_stages_while_recording(void)
{
	struct recording_ui ui;

	recording_ui_init(&ui);
	recording_ui_set_moving(&ui, false);
	CHECK(recording_ui_view(&ui) == UI_VIEW_BEFORE);
	recording_ui_set_moving(&ui, true);
	CHECK(recording_ui_view(&ui) == UI_VIEW_MOVING);
	recording_ui_set_moving(&ui, false);
	CHECK(recording_ui_view(&ui) == UI_VIEW_AFTER);
	recording_ui_set_moving(&ui, true);
	CHECK(recording_ui_view(&ui) == UI_VIEW_MOVING);
}

/* Після паузи запис починається заново: етап знову «до руху», навіть якщо рух був до паузи. */
static void test_resume_after_pause_starts_from_stage_before(void)
{
	struct recording_ui ui;

	recording_ui_init(&ui);
	recording_ui_set_moving(&ui, true);
	recording_ui_set_moving(&ui, false);
	CHECK(recording_ui_view(&ui) == UI_VIEW_AFTER);
	CHECK(recording_ui_press(&ui, 1000)); /* пауза */
	CHECK(recording_ui_press(&ui, 2000)); /* запис знову */
	CHECK(recording_ui_view(&ui) == UI_VIEW_BEFORE);
}

/* На паузі горять усі три незалежно від руху. */
static void test_pause_shows_all_leds_regardless_of_motion(void)
{
	struct recording_ui ui;

	recording_ui_init(&ui);
	CHECK(recording_ui_press(&ui, 1000));
	CHECK(!ui.recording);
	CHECK(recording_ui_view(&ui) == UI_VIEW_PAUSED);
	recording_ui_set_moving(&ui, true);
	CHECK(recording_ui_view(&ui) == UI_VIEW_PAUSED);
}

static void test_presses_toggle_recording(void)
{
	struct recording_ui ui;

	recording_ui_init(&ui);
	CHECK(recording_ui_press(&ui, 1000));
	CHECK(!ui.recording);
	CHECK(recording_ui_press(&ui, 2000));
	CHECK(ui.recording);
	CHECK(recording_ui_view(&ui) == UI_VIEW_BEFORE);
}

/* Дрижання контактів: натискання в межах 200 мс після прийнятого ігнорується. */
static void test_bounce_within_debounce_window_is_ignored(void)
{
	struct recording_ui ui;

	recording_ui_init(&ui);
	CHECK(recording_ui_press(&ui, 1000));
	CHECK(!recording_ui_press(&ui, 1000));
	CHECK(!recording_ui_press(&ui, 1000 + RECORDING_UI_DEBOUNCE_MS - 1));
	CHECK(!ui.recording); /* перемкнуто лише один раз */
	CHECK(recording_ui_press(&ui, 1000 + RECORDING_UI_DEBOUNCE_MS));
	CHECK(ui.recording);
}

/* Відлік вікна дрижання іде від останнього ПРИЙНЯТОГО натискання, а не від ігнорованого. */
static void test_ignored_press_does_not_extend_the_window(void)
{
	struct recording_ui ui;

	recording_ui_init(&ui);
	CHECK(recording_ui_press(&ui, 1000));
	CHECK(!recording_ui_press(&ui, 1150));
	CHECK(recording_ui_press(&ui, 1210));
}

/* Перше натискання приймається навіть у перші мілісекунди роботи (час може бути малим). */
static void test_first_press_is_accepted_at_time_zero(void)
{
	struct recording_ui ui;

	recording_ui_init(&ui);
	CHECK(recording_ui_press(&ui, 0));
	CHECK(!ui.recording);
}

/* Годинник пішов назад: таке натискання ігнорується, а не перемикає запис. */
static void test_press_with_time_going_back_is_ignored(void)
{
	struct recording_ui ui;

	recording_ui_init(&ui);
	CHECK(recording_ui_press(&ui, 5000));
	CHECK(!recording_ui_press(&ui, 100));
	CHECK(!ui.recording);
}

int main(void)
{
	test_initial_state_is_recording_and_still();
	test_view_follows_motion_stages_while_recording();
	test_resume_after_pause_starts_from_stage_before();
	test_pause_shows_all_leds_regardless_of_motion();
	test_presses_toggle_recording();
	test_bounce_within_debounce_window_is_ignored();
	test_ignored_press_does_not_extend_the_window();
	test_first_press_is_accepted_at_time_zero();
	test_press_with_time_going_back_is_ignored();

	if (failures != 0) {
		printf("%d check(s) failed\n", failures);
		return 1;
	}
	printf("recording_ui: all checks passed\n");

	return 0;
}
