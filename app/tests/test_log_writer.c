/**
 * @file test_log_writer.c
 * @brief Перевірки записувача логу: формат рядка, порядок, повнота, зупинка.
 *
 * Запуск: make test (звичайний) або make test-tsan (з перевіркою гонок даних).
 * Код виходу 0 означає, що всі перевірки пройшли. Лог-файли тест створює в /tmp
 * і видаляє сам.
 */
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "check.h"
#include "log_writer.h"
#include "ring_buffer.h"
#include "sample_util.h"

#define SMALL_CAPACITY 8
#define PREFILLED_SAMPLES 1000
#define STREAMED_SAMPLES 100000
#define STREAM_CAPACITY 64
#define TEMP_PATH_SIZE 32
#define SAMPLE_TIME_MS 987654

/* Скільки перевірок не пройшло. main повертає це як код виходу. */
static int failures;

/* Шаблон імені тимчасового файлу для mkstemp. */
static const char TEMP_TEMPLATE[] = "/tmp/test_log_XXXXXX";

/* Створює порожній тимчасовий файл і кладе його шлях у path. */
static void make_temp_path(char *path)
{
	int fd;

	strcpy(path, TEMP_TEMPLATE);
	fd = mkstemp(path);
	CHECK(fd >= 0);
	close(fd);
}

/*
 * Читає лог рядок за рядком і перевіряє: кожен рядок повний і має формат запису з джерелом
 * "sim"; значення йдуть 0, 1, 2, ...; мітки часу не спадають. Повертає кількість рядків.
 */
static size_t check_log_file(const char *path, size_t expected_lines)
{
	char line[LOG_WRITER_LINE_SIZE];
	long long previous_time = 0;
	size_t lines = 0;
	FILE *file = fopen(path, "r");

	CHECK(file != NULL);
	if (file == NULL) {
		return 0;
	}

	while (fgets(line, sizeof(line), file) != NULL) {
		long long time_ms;
		int value;
		size_t length = strlen(line);

		CHECK(length > 0 && line[length - 1] == '\n');
		CHECK(sscanf(line, "{\"t_ms\":%lld,\"src\":\"sim\",\"value\":%d}", &time_ms,
			     &value) == 2);
		CHECK(value == (int)lines);
		CHECK(time_ms >= previous_time);
		previous_time = time_ms;
		lines++;
	}
	fclose(file);
	CHECK(lines == expected_lines);

	return lines;
}

static void test_format_line_produces_exact_json(void)
{
	char line[LOG_WRITER_LINE_SIZE];

	CHECK(log_writer_format_line(line, sizeof(line), "sim", 1234, -7) > 0);
	CHECK(strcmp(line, "{\"t_ms\":1234,\"src\":\"sim\",\"value\":-7}\n") == 0);
}

static void test_format_line_rejects_bad_input(void)
{
	char line[LOG_WRITER_LINE_SIZE];
	char tiny[8];

	CHECK(log_writer_format_line(NULL, sizeof(line), "sim", 0, 0) == -EINVAL);
	CHECK(log_writer_format_line(line, sizeof(line), NULL, 0, 0) == -EINVAL);
	CHECK(log_writer_format_line(line, sizeof(line), "", 0, 0) == -EINVAL);
	CHECK(log_writer_format_line(line, sizeof(line), "Sim", 0, 0) == -EINVAL);
	CHECK(log_writer_format_line(line, sizeof(line), "a\"b", 0, 0) == -EINVAL);
	CHECK(log_writer_format_line(line, sizeof(line), "sixteen_chars_xxx", 0, 0) == -EINVAL);
	CHECK(log_writer_format_line(tiny, sizeof(tiny), "sim", 0, 0) == -ENOSPC);
}

static void test_start_rejects_bad_arguments(void)
{
	struct sample storage[SMALL_CAPACITY];
	struct ring_buffer rb;
	struct log_writer lw;

	CHECK(ring_buffer_init(&rb, storage, SMALL_CAPACITY) == 0);

	CHECK(log_writer_start(NULL, &rb, "/tmp/x") == -EINVAL);
	CHECK(log_writer_start(&lw, NULL, "/tmp/x") == -EINVAL);
	CHECK(log_writer_start(&lw, &rb, NULL) == -EINVAL);
	CHECK(log_writer_stop(NULL, NULL) == -EINVAL);

	ring_buffer_destroy(&rb);
}

static void test_start_reports_unwritable_path(void)
{
	struct sample storage[SMALL_CAPACITY];
	struct ring_buffer rb;
	struct log_writer lw;

	CHECK(ring_buffer_init(&rb, storage, SMALL_CAPACITY) == 0);

	CHECK(log_writer_start(&lw, &rb, "/no_such_dir/log.jsonl") == -ENOENT);

	ring_buffer_destroy(&rb);
}

/* Числа лежать у буфері ще до старту: stop має дописати всі до одного й у тому ж порядку. */
static void test_stop_drains_prefilled_buffer(void)
{
	static struct sample storage[PREFILLED_SAMPLES];
	struct ring_buffer rb;
	struct log_writer lw;
	struct log_writer_stats stats;
	char path[TEMP_PATH_SIZE];
	int32_t i;

	make_temp_path(path);
	CHECK(ring_buffer_init(&rb, storage, PREFILLED_SAMPLES) == 0);
	for (i = 0; i < PREFILLED_SAMPLES; i++) {
		CHECK(push_value(&rb, i) == 0);
	}

	CHECK(log_writer_start(&lw, &rb, path) == 0);
	CHECK(log_writer_stop(&lw, &stats) == 0);

	CHECK(stats.written == PREFILLED_SAMPLES);
	CHECK(stats.failed == 0);
	CHECK(ring_buffer_is_empty(&rb));
	check_log_file(path, PREFILLED_SAMPLES);

	ring_buffer_destroy(&rb);
	unlink(path);
}

/* Дані "тече" з іншого потоку, поки записувач працює: нічого не губиться, порядок збережено. */
static void *push_sequence(void *argument)
{
	struct ring_buffer *rb = argument;
	int32_t i;

	for (i = 0; i < STREAMED_SAMPLES; i++) {
		while (push_value(rb, i) != 0) {
			sched_yield(); /* буфер повний: даємо записувачу забрати числа */
		}
	}

	return NULL;
}

static void test_concurrent_producer_loses_nothing(void)
{
	struct sample storage[STREAM_CAPACITY];
	struct ring_buffer rb;
	struct log_writer lw;
	struct log_writer_stats stats;
	pthread_t producer;
	char path[TEMP_PATH_SIZE];

	make_temp_path(path);
	CHECK(ring_buffer_init(&rb, storage, STREAM_CAPACITY) == 0);
	CHECK(log_writer_start(&lw, &rb, path) == 0);
	CHECK(pthread_create(&producer, NULL, push_sequence, &rb) == 0);

	pthread_join(producer, NULL);
	CHECK(log_writer_stop(&lw, &stats) == 0);

	CHECK(stats.written == STREAMED_SAMPLES);
	CHECK(stats.failed == 0);
	check_log_file(path, STREAMED_SAMPLES);

	ring_buffer_destroy(&rb);
	unlink(path);
}

/* Повторний запуск дописує в кінець наявного файлу, а не затирає його. */
static void test_second_run_appends_to_existing_file(void)
{
	struct sample storage[SMALL_CAPACITY];
	struct ring_buffer rb;
	struct log_writer lw;
	char path[TEMP_PATH_SIZE];
	int run;
	int32_t i;

	make_temp_path(path);
	CHECK(ring_buffer_init(&rb, storage, SMALL_CAPACITY) == 0);

	for (run = 0; run < 2; run++) {
		for (i = 0; i < 3; i++) {
			CHECK(push_value(&rb, run * 3 + i) == 0);
		}
		CHECK(log_writer_start(&lw, &rb, path) == 0);
		CHECK(log_writer_stop(&lw, NULL) == 0);
	}

	check_log_file(path, 6);

	ring_buffer_destroy(&rb);
	unlink(path);
}

static void test_format_event_produces_exact_json(void)
{
	char line[LOG_WRITER_LINE_SIZE];

	CHECK(log_writer_format_event(line, sizeof(line), 20550, MOTION_EVENT_START) > 0);
	CHECK(strcmp(line, "{\"t_ms\":20550,\"src\":\"accel\",\"event\":\"MOVE_START\"}\n") == 0);
	CHECK(log_writer_format_event(line, sizeof(line), 40550, MOTION_EVENT_STOP) > 0);
	CHECK(strcmp(line, "{\"t_ms\":40550,\"src\":\"accel\",\"event\":\"MOVE_STOP\"}\n") == 0);
}

static void test_format_event_rejects_bad_input(void)
{
	char line[LOG_WRITER_LINE_SIZE];
	char tiny[8];

	CHECK(log_writer_format_event(NULL, sizeof(line), 0, MOTION_EVENT_START) == -EINVAL);
	CHECK(log_writer_format_event(line, sizeof(line), 0, MOTION_EVENT_NONE) == -EINVAL);
	CHECK(log_writer_format_event(tiny, sizeof(tiny), 0, MOTION_EVENT_START) == -ENOSPC);
}

/* Рядок з подією: розбирає тип і час; повертає false, якщо рядок не схожий на подію. */
static bool parse_event_line(const char *line, long long *time_ms, char *name, size_t name_size)
{
	char parsed[LOG_WRITER_SOURCE_SIZE];

	if (sscanf(line, "{\"t_ms\":%lld,\"src\":\"accel\",\"event\":\"%15[A-Z_]\"}", time_ms,
		   parsed) != 2) {
		return false;
	}
	snprintf(name, name_size, "%s", parsed);

	return true;
}

#define STILL_SECONDS 2
#define MOVING_SECONDS 3
#define ACCEL_RATE_HZ 100
#define MS_PER_SAMPLE (1000 / ACCEL_RATE_HZ)
#define MOVING_AMPLITUDE_G 0.4

/*
 * Кладе в буфер вигаданий запис: 2 с стою (1 g по Z), 3 с «іду» (Z стрибає між 1,4 і 0,6 g),
 * 2 с знову стою. Повертає кількість відліків і час, з якого починається рух.
 */
static size_t fill_still_moving_still(struct ring_buffer *rb, int64_t *moving_start_ms,
				      int64_t *moving_end_ms)
{
	const size_t total =
	    (size_t)(STILL_SECONDS + MOVING_SECONDS + STILL_SECONDS) * ACCEL_RATE_HZ;
	size_t i;

	*moving_start_ms = STILL_SECONDS * 1000;
	*moving_end_ms = (STILL_SECONDS + MOVING_SECONDS) * 1000;
	for (i = 0; i < total; i++) {
		int64_t time_ms = (int64_t)i * MS_PER_SAMPLE;
		bool moving = time_ms >= *moving_start_ms && time_ms < *moving_end_ms;
		double z_g = 1.0;
		struct sample item;

		if (moving) {
			z_g += (i % 2 == 0) ? MOVING_AMPLITUDE_G : -MOVING_AMPLITUDE_G;
		}
		item = make_accel_item(time_ms, 0.0, 0.0, z_g);
		CHECK(ring_buffer_push(rb, &item) == 0);
	}

	return total;
}

/* Читає лог і перевіряє: рівно два рядки, MOVE_START у межах руху, потім MOVE_STOP після нього. */
static void check_start_then_stop(const char *path, int64_t moving_start_ms, int64_t moving_end_ms)
{
	char line[LOG_WRITER_LINE_SIZE];
	char name[LOG_WRITER_SOURCE_SIZE];
	long long start_ms = -1;
	long long stop_ms = -1;
	FILE *file = fopen(path, "r");

	CHECK(file != NULL);
	if (file == NULL) {
		return;
	}

	CHECK(fgets(line, sizeof(line), file) != NULL);
	CHECK(parse_event_line(line, &start_ms, name, sizeof(name)));
	CHECK(strcmp(name, "MOVE_START") == 0);
	CHECK(start_ms > moving_start_ms && start_ms < moving_end_ms);

	CHECK(fgets(line, sizeof(line), file) != NULL);
	CHECK(parse_event_line(line, &stop_ms, name, sizeof(name)));
	CHECK(strcmp(name, "MOVE_STOP") == 0);
	CHECK(stop_ms > moving_end_ms);

	CHECK(fgets(line, sizeof(line), file) == NULL); /* більше нічого: відліки в лог не йдуть */
	fclose(file);
}

/* Головна перевірка кроку 5.3: «стою, іду, стою» дає в лозі рівно MOVE_START і MOVE_STOP. */
static void test_accel_samples_produce_start_and_stop_events(void)
{
	static struct sample
	    storage[(STILL_SECONDS + MOVING_SECONDS + STILL_SECONDS) * ACCEL_RATE_HZ];
	struct ring_buffer rb;
	struct log_writer lw;
	struct log_writer_stats stats;
	int64_t moving_start_ms;
	int64_t moving_end_ms;
	size_t samples;
	char path[TEMP_PATH_SIZE];

	make_temp_path(path);
	CHECK(ring_buffer_init(&rb, storage, sizeof(storage) / sizeof(storage[0])) == 0);
	samples = fill_still_moving_still(&rb, &moving_start_ms, &moving_end_ms);
	CHECK(samples == sizeof(storage) / sizeof(storage[0]));

	CHECK(log_writer_start(&lw, &rb, path) == 0);
	CHECK(log_writer_stop(&lw, &stats) == 0);

	CHECK(stats.written == 2);
	CHECK(stats.events == 2);
	CHECK(stats.failed == 0);
	CHECK(ring_buffer_is_empty(&rb));
	check_start_then_stop(path, moving_start_ms, moving_end_ms);

	ring_buffer_destroy(&rb);
	unlink(path);
}

/* Відлік із часом, що спадає, відкидається й рахується як помилка; решта працює далі. */
static void test_accel_sample_with_decreasing_time_counts_as_failed(void)
{
	struct sample storage[SMALL_CAPACITY];
	struct ring_buffer rb;
	struct log_writer lw;
	struct log_writer_stats stats;
	struct sample item;
	char path[TEMP_PATH_SIZE];

	make_temp_path(path);
	CHECK(ring_buffer_init(&rb, storage, SMALL_CAPACITY) == 0);
	item = make_accel_item(1000, 0.0, 0.0, 1.0);
	CHECK(ring_buffer_push(&rb, &item) == 0);
	item = make_accel_item(500, 0.0, 0.0, 1.0);
	CHECK(ring_buffer_push(&rb, &item) == 0);

	CHECK(log_writer_start(&lw, &rb, path) == 0);
	CHECK(log_writer_stop(&lw, &stats) == 0);

	CHECK(stats.failed == 1);
	CHECK(stats.written == 0);

	ring_buffer_destroy(&rb);
	unlink(path);
}

/* Числа імітатора й відліки акселерометра в одному буфері не заважають один одному. */
static void test_sim_and_accel_samples_share_the_log(void)
{
	struct sample storage[SMALL_CAPACITY];
	struct ring_buffer rb;
	struct log_writer lw;
	struct log_writer_stats stats;
	struct sample item;
	char path[TEMP_PATH_SIZE];

	make_temp_path(path);
	CHECK(ring_buffer_init(&rb, storage, SMALL_CAPACITY) == 0);
	CHECK(push_value(&rb, 0) == 0);
	item = make_accel_item(0, 0.0, 0.0, 1.0);
	CHECK(ring_buffer_push(&rb, &item) == 0);
	CHECK(push_value(&rb, 1) == 0);

	CHECK(log_writer_start(&lw, &rb, path) == 0);
	CHECK(log_writer_stop(&lw, &stats) == 0);

	CHECK(stats.written == 2); /* два числа імітатора; відлік у спокої подій не дав */
	CHECK(stats.events == 0);
	CHECK(stats.failed == 0);
	check_log_file(path, 2);

	ring_buffer_destroy(&rb);
	unlink(path);
}

/* Число імітатора потрапляє в лог із тією міткою часу, з якою воно прийшло, а не з часом запису. */
static void test_sim_sample_keeps_its_own_timestamp(void)
{
	struct sample storage[SMALL_CAPACITY];
	struct ring_buffer rb;
	struct log_writer lw;
	struct sample item = make_sim_sample(42);
	char path[TEMP_PATH_SIZE];
	char line[LOG_WRITER_LINE_SIZE];
	FILE *file;

	item.time_ms = SAMPLE_TIME_MS;
	make_temp_path(path);
	CHECK(ring_buffer_init(&rb, storage, SMALL_CAPACITY) == 0);
	CHECK(ring_buffer_push(&rb, &item) == 0);

	CHECK(log_writer_start(&lw, &rb, path) == 0);
	CHECK(log_writer_stop(&lw, NULL) == 0);

	file = fopen(path, "r");
	CHECK(file != NULL);
	if (file != NULL) {
		CHECK(fgets(line, sizeof(line), file) != NULL);
		CHECK(strcmp(line, "{\"t_ms\":987654,\"src\":\"sim\",\"value\":42}\n") == 0);
		fclose(file);
	}

	ring_buffer_destroy(&rb);
	unlink(path);
}

int main(void)
{
	test_format_line_produces_exact_json();
	test_format_line_rejects_bad_input();
	test_format_event_produces_exact_json();
	test_format_event_rejects_bad_input();
	test_start_rejects_bad_arguments();
	test_start_reports_unwritable_path();
	test_stop_drains_prefilled_buffer();
	test_concurrent_producer_loses_nothing();
	test_second_run_appends_to_existing_file();
	test_accel_samples_produce_start_and_stop_events();
	test_accel_sample_with_decreasing_time_counts_as_failed();
	test_sim_and_accel_samples_share_the_log();
	test_sim_sample_keeps_its_own_timestamp();

	if (failures != 0) {
		printf("%d check(s) failed\n", failures);
		return 1;
	}
	printf("log_writer: all checks passed\n");

	return 0;
}
