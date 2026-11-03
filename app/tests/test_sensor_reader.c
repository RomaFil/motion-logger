/**
 * @file test_sensor_reader.c
 * @brief Перевірки читача датчика sensor_reader на «підставному» датчику (без файлів і заліза).
 *
 * Підставний датчик віддає заздалегідь задані відліки й за потреби імітує збої. Саме так
 * перевіряється, що ядро працює з інтерфейсом struct sensor, а не з конкретним драйвером.
 *
 * Запуск: make test (звичайний) або make test-tsan. Код виходу 0 означає, що все пройшло.
 */
#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "check.h"
#include "ring_buffer.h"
#include "sensor.h"
#include "sensor_reader.h"

#define SAMPLE_COUNT 5
#define SMALL_CAPACITY 3
#define BUFFER_CAPACITY 16
#define NEVER_FAIL SIZE_MAX

/* Скільки перевірок не пройшло. main повертає це як код виходу. */
static int failures;

/* Датчик-підробка: віддає samples по черзі; на кроці fail_at повертає fail_code. */
struct fake_sensor {
	struct sensor base; /* перше поле, як вимагає інтерфейс */
	struct accel_sample samples[SAMPLE_COUNT];
	size_t next;
	size_t read_calls;
	size_t fail_at; /* номер відліку, перед яким повернути fail_code (NEVER_FAIL = ніколи) */
	int fail_code;
	size_t fail_count;    /* скільки разів підряд повернути fail_code, далі віддавати відліки */
	bool interrupt_first; /* перший виклик read повертає -EINTR, не віддавши відліку */
};

static int fake_read(struct sensor *self, struct accel_sample *out)
{
	struct fake_sensor *fake = (struct fake_sensor *)self;

	fake->read_calls++;
	if (fake->interrupt_first && fake->read_calls == 1) {
		return -EINTR;
	}
	if (fake->next == fake->fail_at && fake->fail_count > 0) {
		fake->fail_count--;
		return fake->fail_code;
	}
	if (fake->next >= SAMPLE_COUNT) {
		return -ENODATA;
	}
	*out = fake->samples[fake->next++];

	return 0;
}

/* Готує датчик: відліки з часом 0, 10, 20 ... мс і x = 0,1 * номер, y = -номер, z = 1. */
static void init_fake(struct fake_sensor *fake)
{
	size_t i;

	fake->base.read = fake_read;
	fake->base.close = NULL;
	fake->next = 0;
	fake->read_calls = 0;
	fake->fail_at = NEVER_FAIL;
	fake->fail_code = 0;
	fake->fail_count = SIZE_MAX;
	fake->interrupt_first = false;
	for (i = 0; i < SAMPLE_COUNT; i++) {
		fake->samples[i].time_ms = (int64_t)i * 10;
		fake->samples[i].x_g = 0.1 * (double)i;
		fake->samples[i].y_g = -(double)i;
		fake->samples[i].z_g = 1.0;
	}
}

static void test_run_rejects_bad_arguments(void)
{
	static volatile sig_atomic_t never_stop;
	struct sample storage[SMALL_CAPACITY];
	struct ring_buffer rb;
	struct fake_sensor fake;
	struct sensor_reader_stats stats;
	struct sensor no_read = {.read = NULL};

	CHECK(ring_buffer_init(&rb, storage, SMALL_CAPACITY) == 0);
	init_fake(&fake);

	CHECK(sensor_reader_run(NULL, SAMPLE_SOURCE_ACCEL, &rb, &never_stop, &stats) == -EINVAL);
	CHECK(sensor_reader_run(&no_read, SAMPLE_SOURCE_ACCEL, &rb, &never_stop, &stats) ==
	      -EINVAL);
	CHECK(sensor_reader_run(&fake.base, SAMPLE_SOURCE_ACCEL, NULL, &never_stop, &stats) ==
	      -EINVAL);
	CHECK(sensor_reader_run(&fake.base, SAMPLE_SOURCE_ACCEL, &rb, NULL, &stats) == -EINVAL);
	CHECK(sensor_reader_run(&fake.base, SAMPLE_SOURCE_ACCEL, &rb, &never_stop, NULL) ==
	      -EINVAL);

	ring_buffer_destroy(&rb);
}

/* Усі відліки доходять у тому ж порядку, з усіма полями й джерелом ACCEL. */
static void test_samples_arrive_in_order_with_all_fields(void)
{
	static volatile sig_atomic_t never_stop;
	struct sample storage[BUFFER_CAPACITY];
	struct ring_buffer rb;
	struct fake_sensor fake;
	struct sensor_reader_stats stats;
	size_t i;

	CHECK(ring_buffer_init(&rb, storage, BUFFER_CAPACITY) == 0);
	init_fake(&fake);

	CHECK(sensor_reader_run(&fake.base, SAMPLE_SOURCE_ACCEL, &rb, &never_stop, &stats) == 0);
	CHECK(stats.received == SAMPLE_COUNT);
	CHECK(stats.dropped == 0);

	for (i = 0; i < SAMPLE_COUNT; i++) {
		struct sample item;

		CHECK(ring_buffer_pop(&rb, &item) == 0);
		CHECK(item.source == SAMPLE_SOURCE_ACCEL);
		CHECK(item.time_ms == fake.samples[i].time_ms);
		CHECK(item.x_g == fake.samples[i].x_g);
		CHECK(item.y_g == fake.samples[i].y_g);
		CHECK(item.z_g == fake.samples[i].z_g);
	}
	CHECK(ring_buffer_is_empty(&rb));

	ring_buffer_destroy(&rb);
}

/* -EINTR від датчика не помилка й не втрата відліку: читач пробує ще раз. */
static void test_interrupted_read_is_retried(void)
{
	static volatile sig_atomic_t never_stop;
	struct sample storage[BUFFER_CAPACITY];
	struct ring_buffer rb;
	struct fake_sensor fake;
	struct sensor_reader_stats stats;

	CHECK(ring_buffer_init(&rb, storage, BUFFER_CAPACITY) == 0);
	init_fake(&fake);
	fake.interrupt_first = true;

	CHECK(sensor_reader_run(&fake.base, SAMPLE_SOURCE_ACCEL, &rb, &never_stop, &stats) == 0);
	CHECK(stats.received == SAMPLE_COUNT);

	ring_buffer_destroy(&rb);
}

/* Постійна помилка датчика (не кінець даних і не збій шини) іде нагору одразу, а вже прийняті
 * відліки лишаються в підсумку. */
static void test_sensor_error_is_returned(void)
{
	static volatile sig_atomic_t never_stop;
	struct sample storage[BUFFER_CAPACITY];
	struct ring_buffer rb;
	struct fake_sensor fake;
	struct sensor_reader_stats stats;

	CHECK(ring_buffer_init(&rb, storage, BUFFER_CAPACITY) == 0);
	init_fake(&fake);
	fake.fail_at = 2;
	fake.fail_code = -EPROTO;

	CHECK(sensor_reader_run(&fake.base, SAMPLE_SOURCE_ACCEL, &rb, &never_stop, &stats) ==
	      -EPROTO);
	CHECK(stats.received == 2);
	CHECK(stats.errors == 0);
	CHECK(fake.read_calls == 3);
	CHECK(ring_buffer_count(&rb) == 2);

	ring_buffer_destroy(&rb);
}

/* Кілька збоїв шини підряд (-EIO, -EREMOTEIO): відліки пропущено, запис іде далі. */
static void test_transient_errors_are_skipped(void)
{
	static volatile sig_atomic_t never_stop;
	static const int codes[] = {-EIO, -EREMOTEIO, -ETIMEDOUT, -EAGAIN, -EBUSY};
	size_t c;

	for (c = 0; c < sizeof(codes) / sizeof(codes[0]); c++) {
		struct sample storage[BUFFER_CAPACITY];
		struct ring_buffer rb;
		struct fake_sensor fake;
		struct sensor_reader_stats stats;

		CHECK(ring_buffer_init(&rb, storage, BUFFER_CAPACITY) == 0);
		init_fake(&fake);
		fake.fail_at = 2;
		fake.fail_code = codes[c];
		fake.fail_count = 3;

		CHECK(sensor_reader_run(&fake.base, SAMPLE_SOURCE_ACCEL, &rb, &never_stop,
					&stats) == 0);
		CHECK(stats.received == SAMPLE_COUNT);
		CHECK(stats.errors == 3);
		CHECK(ring_buffer_count(&rb) == SAMPLE_COUNT);

		ring_buffer_destroy(&rb);
	}
}

/* Датчик зник зовсім: після SENSOR_READER_MAX_CONSECUTIVE_ERRORS збоїв підряд читач здається. */
static void test_endless_transient_errors_give_up(void)
{
	static volatile sig_atomic_t never_stop;
	struct sample storage[BUFFER_CAPACITY];
	struct ring_buffer rb;
	struct fake_sensor fake;
	struct sensor_reader_stats stats;

	CHECK(ring_buffer_init(&rb, storage, BUFFER_CAPACITY) == 0);
	init_fake(&fake);
	fake.fail_at = 1;
	fake.fail_code = -EIO;

	CHECK(sensor_reader_run(&fake.base, SAMPLE_SOURCE_ACCEL, &rb, &never_stop, &stats) == -EIO);
	CHECK(stats.received == 1);
	CHECK(stats.errors == SENSOR_READER_MAX_CONSECUTIVE_ERRORS);

	ring_buffer_destroy(&rb);
}

/* Лічильник «підряд» скидається після вдалого відліку: розкидані збої не накопичуються. */
static void test_error_streak_resets_after_success(void)
{
	static volatile sig_atomic_t never_stop;
	struct sample storage[BUFFER_CAPACITY];
	struct ring_buffer rb;
	struct fake_sensor fake;
	struct sensor_reader_stats stats;

	CHECK(ring_buffer_init(&rb, storage, BUFFER_CAPACITY) == 0);
	init_fake(&fake);
	fake.fail_at = 1;
	fake.fail_code = -EIO;
	fake.fail_count = SENSOR_READER_MAX_CONSECUTIVE_ERRORS - 1;

	CHECK(sensor_reader_run(&fake.base, SAMPLE_SOURCE_ACCEL, &rb, &never_stop, &stats) == 0);
	CHECK(stats.received == SAMPLE_COUNT);
	CHECK(stats.errors == SENSOR_READER_MAX_CONSECUTIVE_ERRORS - 1);

	ring_buffer_destroy(&rb);
}

/* Повний буфер: старі відліки лишаються, зайві рахуються як втрата. */
static void test_full_buffer_counts_dropped_and_keeps_oldest(void)
{
	static volatile sig_atomic_t never_stop;
	struct sample storage[SMALL_CAPACITY];
	struct ring_buffer rb;
	struct fake_sensor fake;
	struct sensor_reader_stats stats;
	struct sample item;

	CHECK(ring_buffer_init(&rb, storage, SMALL_CAPACITY) == 0);
	init_fake(&fake);

	CHECK(sensor_reader_run(&fake.base, SAMPLE_SOURCE_ACCEL, &rb, &never_stop, &stats) == 0);
	CHECK(stats.received == SAMPLE_COUNT);
	CHECK(stats.dropped == SAMPLE_COUNT - SMALL_CAPACITY);

	CHECK(ring_buffer_pop(&rb, &item) == 0);
	CHECK(item.time_ms == 0);

	ring_buffer_destroy(&rb);
}

/* Тип джерела з параметра потрапляє в кожен запис (додатковий акселерометр відрізняється від
 * основного). */
static void test_source_parameter_is_stored_in_every_sample(void)
{
	static volatile sig_atomic_t never_stop;
	struct sample storage[BUFFER_CAPACITY];
	struct ring_buffer rb;
	struct fake_sensor fake;
	struct sensor_reader_stats stats;
	struct sample item;
	size_t i;

	CHECK(ring_buffer_init(&rb, storage, BUFFER_CAPACITY) == 0);
	init_fake(&fake);

	CHECK(sensor_reader_run(&fake.base, SAMPLE_SOURCE_ACCEL2, &rb, &never_stop, &stats) == 0);
	for (i = 0; i < SAMPLE_COUNT; i++) {
		CHECK(ring_buffer_pop(&rb, &item) == 0);
		CHECK(item.source == SAMPLE_SOURCE_ACCEL2);
	}

	ring_buffer_destroy(&rb);
}

/* Прапорець зупинки вже піднято: датчик не опитується взагалі. */
static void test_stop_flag_prevents_any_read(void)
{
	static volatile sig_atomic_t already_stopped = 1;
	struct sample storage[SMALL_CAPACITY];
	struct ring_buffer rb;
	struct fake_sensor fake;
	struct sensor_reader_stats stats;

	CHECK(ring_buffer_init(&rb, storage, SMALL_CAPACITY) == 0);
	init_fake(&fake);

	CHECK(sensor_reader_run(&fake.base, SAMPLE_SOURCE_ACCEL, &rb, &already_stopped, &stats) ==
	      0);
	CHECK(stats.received == 0);
	CHECK(fake.read_calls == 0);

	ring_buffer_destroy(&rb);
}

int main(void)
{
	test_run_rejects_bad_arguments();
	test_samples_arrive_in_order_with_all_fields();
	test_interrupted_read_is_retried();
	test_sensor_error_is_returned();
	test_transient_errors_are_skipped();
	test_endless_transient_errors_give_up();
	test_error_streak_resets_after_success();
	test_full_buffer_counts_dropped_and_keeps_oldest();
	test_source_parameter_is_stored_in_every_sample();
	test_stop_flag_prevents_any_read();

	if (failures != 0) {
		printf("%d check(s) failed\n", failures);
		return 1;
	}
	printf("sensor_reader: all checks passed\n");

	return 0;
}
