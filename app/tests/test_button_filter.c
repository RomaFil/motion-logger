/**
 * @file test_button_filter.c
 * @brief Перевірки фільтра кнопки (core/button_filter): натискання це лише перехід «відпущено →
 * натиснуто».
 *
 * Запуск: make test (звичайний) або make test-tsan. Код виходу 0 означає, що все пройшло.
 */
#include <stdio.h>

#include "button_filter.h"
#include "check.h"

/* Скільки перевірок не пройшло. main повертає це як код виходу. */
static int failures;

static void test_first_active_level_is_a_press(void)
{
	struct button_filter filter;

	button_filter_init(&filter);
	CHECK(button_filter_update(&filter, true));
}

static void test_released_level_is_never_a_press(void)
{
	struct button_filter filter;

	button_filter_init(&filter);
	CHECK(!button_filter_update(&filter, false));
	CHECK(!button_filter_update(&filter, false));
}

/* Кнопку тримають: повторні події з тим самим рівнем не дають нових натискань. */
static void test_held_button_gives_one_press(void)
{
	struct button_filter filter;

	button_filter_init(&filter);
	CHECK(button_filter_update(&filter, true));
	CHECK(!button_filter_update(&filter, true));
	CHECK(!button_filter_update(&filter, true));
}

/*
 * Регресія 05.11.2026: після довгого утримання відпускання давало ще одну подію (дрижання
 * контакту), і пауза знімалась. Після заспокоєння контакт відпущений, тож нового натискання бути не
 * може.
 */
static void test_release_bounce_settling_released_is_not_a_press(void)
{
	struct button_filter filter;

	button_filter_init(&filter);
	CHECK(button_filter_update(&filter, true));
	CHECK(!button_filter_update(&filter, false)); /* відпустили */
	CHECK(!button_filter_update(&filter, false)); /* подія дрижання, рівень уже відпущений */
	CHECK(!button_filter_update(&filter, false));
}

/* Після відпускання наступне натискання знову приймається. */
static void test_press_after_release_is_accepted_again(void)
{
	struct button_filter filter;

	button_filter_init(&filter);
	CHECK(button_filter_update(&filter, true));
	CHECK(!button_filter_update(&filter, false));
	CHECK(button_filter_update(&filter, true));
	CHECK(!button_filter_update(&filter, true));
	CHECK(!button_filter_update(&filter, false));
	CHECK(button_filter_update(&filter, true));
}

int main(void)
{
	test_first_active_level_is_a_press();
	test_released_level_is_never_a_press();
	test_held_button_gives_one_press();
	test_release_bounce_settling_released_is_not_a_press();
	test_press_after_release_is_accepted_again();

	if (failures != 0) {
		printf("%d check(s) failed\n", failures);
		return 1;
	}
	printf("button_filter: all checks passed\n");

	return 0;
}
