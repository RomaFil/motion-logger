/**
 * @file test_ring_buffer.c
 * @brief Перевірки кільцевого буфера без жодних фреймворків: звичайний C.
 *
 * Запуск: make test (звичайний) або make test-tsan (з перевіркою гонок даних).
 * Код виходу 0 означає, що всі перевірки пройшли. Тести не потребують заліза
 * й працюють на ноутбуці.
 */
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "check.h"
#include "ring_buffer.h"

#define TEN_ITEMS 10
#define SMALL_CAPACITY 3
#define WRAP_CAPACITY 4

/* Значення, яке pop не повинен чіпати, якщо нічого не віддав. */
#define UNTOUCHED 12345

/* Скільки чисел проходить через буфер за один раунд двопотокового тесту. */
#define BIG_COUNT 1000000

/* Буфер малий навмисно: потоки часто впираються в "повний" і "порожній". */
#define BIG_CAPACITY 64

/* Скільки раундів поспіль без жодної втрати. */
#define ROUNDS 10

/* Скільки перевірок не пройшло. main повертає це як код виходу. */
static int failures;

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
	ring_buffer_destroy(&rb);
}

static void test_pop_from_empty_returns_nodata(void)
{
	struct ring_buffer rb;
	int32_t storage[SMALL_CAPACITY];
	int32_t value = UNTOUCHED;

	CHECK(ring_buffer_init(&rb, storage, SMALL_CAPACITY) == 0);
	CHECK(ring_buffer_pop(&rb, &value) == -ENODATA);
	CHECK(value == UNTOUCHED);
	ring_buffer_destroy(&rb);
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
	ring_buffer_destroy(&rb);
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
	ring_buffer_destroy(&rb);
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
	ring_buffer_destroy(&rb);
}

/* Результат читача: його заповнює потік-читач, а перевіряє основний потік після join. */
struct reader_report {
	struct ring_buffer *rb;
	int64_t sum;   /* сума всіх прочитаних чисел */
	bool order_ok; /* true, якщо числа прийшли рівно в тому порядку, в якому їх поклали */
};

/*
 * Потік-писар: кладе числа 0, 1, 2, ... BIG_COUNT-1. Якщо буфер повний, віддає процесор
 * іншому потоку (sched_yield) і пробує знову. Так робимо лише в тесті: у справжній програмі
 * очікування буде спокійнішим (цикл подій epoll, крок 2.4).
 */
static void *write_numbers(void *argument)
{
	struct ring_buffer *rb = argument;
	int32_t number;
	int result;

	for (number = 0; number < BIG_COUNT; number++) {
		result = ring_buffer_push(rb, number);
		while (result == -ENOSPC) {
			sched_yield();
			result = ring_buffer_push(rb, number);
		}
	}

	return NULL;
}

/*
 * Потік-читач: забирає рівно BIG_COUNT чисел, перевіряє порядок і складає суму. Якщо число
 * не те, що очікувалось, лише запам'ятовує це й продовжує забирати: тоді писар не зависне
 * на повному буфері, і тест завершиться, а не повисне.
 */
static void *read_numbers(void *argument)
{
	struct reader_report *report = argument;
	int32_t expected;
	int32_t value;
	int result;

	report->sum = 0;
	report->order_ok = true;
	for (expected = 0; expected < BIG_COUNT; expected++) {
		result = ring_buffer_pop(report->rb, &value);
		while (result == -ENODATA) {
			sched_yield();
			result = ring_buffer_pop(report->rb, &value);
		}
		if (result != 0 || value != expected) {
			report->order_ok = false;
		}
		report->sum += value;
	}

	return NULL;
}

/* Один раунд: два потоки, мільйон чисел. Перевіряє порядок, суму й порожній буфер у кінці. */
static void run_two_thread_round(void)
{
	struct ring_buffer rb;
	int32_t storage[BIG_CAPACITY];
	struct reader_report report = {.rb = &rb};
	pthread_t writer;
	pthread_t reader;
	const int64_t expected_sum = (int64_t)BIG_COUNT * (BIG_COUNT - 1) / 2;

	CHECK(ring_buffer_init(&rb, storage, BIG_CAPACITY) == 0);
	CHECK(pthread_create(&writer, NULL, write_numbers, &rb) == 0);
	CHECK(pthread_create(&reader, NULL, read_numbers, &report) == 0);
	CHECK(pthread_join(writer, NULL) == 0);
	CHECK(pthread_join(reader, NULL) == 0);

	CHECK(report.order_ok);
	CHECK(report.sum == expected_sum);
	CHECK(ring_buffer_is_empty(&rb));
	ring_buffer_destroy(&rb);
}

/* Головна перевірка кроку 2.3: ROUNDS раундів поспіль без жодної втрати. */
static void test_two_threads_keep_order_and_sum(void)
{
	int round;

	for (round = 0; round < ROUNDS; round++) {
		run_two_thread_round();
	}
}

int main(void)
{
	test_init_rejects_bad_arguments();
	test_new_buffer_is_empty();
	test_pop_from_empty_returns_nodata();
	test_push_ten_pop_ten_keeps_order();
	test_push_into_full_returns_nospc_and_keeps_data();
	test_wrap_around_keeps_order();
	test_two_threads_keep_order_and_sum();

	if (failures != 0) {
		printf("%d check(s) failed\n", failures);
		return 1;
	}
	printf("ring_buffer: all checks passed\n");
	return 0;
}
