/**
 * @file test_ring_buffer.c
 * @brief Перевірки кільцевого буфера без жодних фреймворків: звичайний C.
 *
 * Запуск: make test (у каталозі app). Код виходу 0 означає, що всі перевірки пройшли.
 * Тести не потребують заліза й працюють на ноутбуці.
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>

#include "ring_buffer.h"

#define TEN_ITEMS 10
#define SMALL_CAPACITY 3
#define WRAP_CAPACITY 4
#define UNTOUCHED 12345 /* значення, яке pop не повинен чіпати, якщо нічого не віддав */

/* Скільки перевірок не пройшло. main повертає це як код виходу. */
static int failures;

/*
 * Перевіряє умову. Якщо вона хибна, друкує файл, рядок і сам вираз та рахує помилку.
 * Тест після цього не зупиняється: так за один запуск видно всі проблеми.
 */
#define CHECK(condition)                                                                           \
	do {                                                                                       \
		if (!(condition)) {                                                                \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);                \
			failures++;                                                                \
		}                                                                                  \
	} while (0)

/* Забирає одне число і перевіряє, що операція вдалась і число таке, як очікували. */
static void check_pop_equals(struct ring_buffer *rb, int32_t expected)
{
	int32_t value = 0;

	CHECK(ring_buffer_pop(rb, &value) == 0);
	CHECK(value == expected);
}

static void test_init_rejects_bad_arguments(void)
{
	struct ring_buffer rb;
	int32_t storage[SMALL_CAPACITY];

	CHECK(ring_buffer_init(NULL, storage, SMALL_CAPACITY) == -EINVAL);
	CHECK(ring_buffer_init(&rb, NULL, SMALL_CAPACITY) == -EINVAL);
	CHECK(ring_buffer_init(&rb, storage, 0) == -EINVAL);
	CHECK(ring_buffer_push(NULL, 1) == -EINVAL);
	CHECK(ring_buffer_pop(NULL, &(int32_t){0}) == -EINVAL);
}

static void test_new_buffer_is_empty(void)
{
	struct ring_buffer rb;
	int32_t storage[SMALL_CAPACITY];

	CHECK(ring_buffer_init(&rb, storage, SMALL_CAPACITY) == 0);
	CHECK(ring_buffer_is_empty(&rb));
	CHECK(!ring_buffer_is_full(&rb));
	CHECK(ring_buffer_count(&rb) == 0);
}

static void test_pop_from_empty_returns_nodata(void)
{
	struct ring_buffer rb;
	int32_t storage[SMALL_CAPACITY];
	int32_t value = UNTOUCHED;

	CHECK(ring_buffer_init(&rb, storage, SMALL_CAPACITY) == 0);
	CHECK(ring_buffer_pop(&rb, &value) == -ENODATA);
	CHECK(value == UNTOUCHED);
}

/* Головна перевірка кроку 2.2: поклав 10 чисел, взяв 10, порядок той самий. */
static void test_push_ten_pop_ten_keeps_order(void)
{
	struct ring_buffer rb;
	int32_t storage[TEN_ITEMS];
	int32_t number;

	CHECK(ring_buffer_init(&rb, storage, TEN_ITEMS) == 0);
	for (number = 0; number < TEN_ITEMS; number++) {
		CHECK(ring_buffer_push(&rb, number * 10) == 0);
	}
	CHECK(ring_buffer_is_full(&rb));
	CHECK(ring_buffer_count(&rb) == TEN_ITEMS);

	for (number = 0; number < TEN_ITEMS; number++) {
		check_pop_equals(&rb, number * 10);
	}
	CHECK(ring_buffer_is_empty(&rb));
}

static void test_push_into_full_returns_nospc_and_keeps_data(void)
{
	struct ring_buffer rb;
	int32_t storage[SMALL_CAPACITY];

	CHECK(ring_buffer_init(&rb, storage, SMALL_CAPACITY) == 0);
	CHECK(ring_buffer_push(&rb, 1) == 0);
	CHECK(ring_buffer_push(&rb, 2) == 0);
	CHECK(ring_buffer_push(&rb, 3) == 0);

	CHECK(ring_buffer_push(&rb, 4) == -ENOSPC);
	CHECK(ring_buffer_count(&rb) == SMALL_CAPACITY);

	/* Число 4 не затерло жодне старе число. */
	check_pop_equals(&rb, 1);
	check_pop_equals(&rb, 2);
	check_pop_equals(&rb, 3);
}

/*
 * Перевірка повороту через кінець масиву: після двох pop індекс читання вже посунувся,
 * тому наступні push заходять за кінець масиву і повертаються на початок.
 */
static void test_wrap_around_keeps_order(void)
{
	struct ring_buffer rb;
	int32_t storage[WRAP_CAPACITY];

	CHECK(ring_buffer_init(&rb, storage, WRAP_CAPACITY) == 0);
	CHECK(ring_buffer_push(&rb, 1) == 0);
	CHECK(ring_buffer_push(&rb, 2) == 0);
	CHECK(ring_buffer_push(&rb, 3) == 0);
	check_pop_equals(&rb, 1);
	check_pop_equals(&rb, 2);

	CHECK(ring_buffer_push(&rb, 4) == 0);
	CHECK(ring_buffer_push(&rb, 5) == 0);
	CHECK(ring_buffer_push(&rb, 6) == 0);
	CHECK(ring_buffer_is_full(&rb));

	check_pop_equals(&rb, 3);
	check_pop_equals(&rb, 4);
	check_pop_equals(&rb, 5);
	check_pop_equals(&rb, 6);
	CHECK(ring_buffer_is_empty(&rb));
}

int main(void)
{
	test_init_rejects_bad_arguments();
	test_new_buffer_is_empty();
	test_pop_from_empty_returns_nodata();
	test_push_ten_pop_ten_keeps_order();
	test_push_into_full_returns_nospc_and_keeps_data();
	test_wrap_around_keeps_order();

	if (failures != 0) {
		printf("%d check(s) failed\n", failures);
		return 1;
	}
	printf("ring_buffer: all checks passed\n");
	return 0;
}
