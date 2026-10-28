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
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "check.h"
#include "log_writer.h"
#include "ring_buffer.h"

#define SMALL_CAPACITY 8
#define PREFILLED_SAMPLES 1000
#define STREAMED_SAMPLES 100000
#define STREAM_CAPACITY 64
#define TEMP_PATH_SIZE 32

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
 * "test"; значення йдуть 0, 1, 2, ...; мітки часу не спадають. Повертає кількість рядків.
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
		CHECK(sscanf(line, "{\"t_ms\":%lld,\"src\":\"test\",\"value\":%d}", &time_ms,
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
	int32_t storage[SMALL_CAPACITY];
	struct ring_buffer rb;
	struct log_writer lw;

	CHECK(ring_buffer_init(&rb, storage, SMALL_CAPACITY) == 0);

	CHECK(log_writer_start(NULL, &rb, "test", "/tmp/x") == -EINVAL);
	CHECK(log_writer_start(&lw, NULL, "test", "/tmp/x") == -EINVAL);
	CHECK(log_writer_start(&lw, &rb, "BAD NAME", "/tmp/x") == -EINVAL);
	CHECK(log_writer_start(&lw, &rb, "test", NULL) == -EINVAL);
	CHECK(log_writer_stop(NULL, NULL) == -EINVAL);

	ring_buffer_destroy(&rb);
}

static void test_start_reports_unwritable_path(void)
{
	int32_t storage[SMALL_CAPACITY];
	struct ring_buffer rb;
	struct log_writer lw;

	CHECK(ring_buffer_init(&rb, storage, SMALL_CAPACITY) == 0);

	CHECK(log_writer_start(&lw, &rb, "test", "/no_such_dir/log.jsonl") == -ENOENT);

	ring_buffer_destroy(&rb);
}

/* Числа лежать у буфері ще до старту: stop має дописати всі до одного й у тому ж порядку. */
static void test_stop_drains_prefilled_buffer(void)
{
	static int32_t storage[PREFILLED_SAMPLES];
	struct ring_buffer rb;
	struct log_writer lw;
	struct log_writer_stats stats;
	char path[TEMP_PATH_SIZE];
	int32_t i;

	make_temp_path(path);
	CHECK(ring_buffer_init(&rb, storage, PREFILLED_SAMPLES) == 0);
	for (i = 0; i < PREFILLED_SAMPLES; i++) {
		CHECK(ring_buffer_push(&rb, i) == 0);
	}

	CHECK(log_writer_start(&lw, &rb, "test", path) == 0);
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
		while (ring_buffer_push(rb, i) != 0) {
			sched_yield(); /* буфер повний: даємо записувачу забрати числа */
		}
	}

	return NULL;
}

static void test_concurrent_producer_loses_nothing(void)
{
	int32_t storage[STREAM_CAPACITY];
	struct ring_buffer rb;
	struct log_writer lw;
	struct log_writer_stats stats;
	pthread_t producer;
	char path[TEMP_PATH_SIZE];

	make_temp_path(path);
	CHECK(ring_buffer_init(&rb, storage, STREAM_CAPACITY) == 0);
	CHECK(log_writer_start(&lw, &rb, "test", path) == 0);
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
	int32_t storage[SMALL_CAPACITY];
	struct ring_buffer rb;
	struct log_writer lw;
	char path[TEMP_PATH_SIZE];
	int run;
	int32_t i;

	make_temp_path(path);
	CHECK(ring_buffer_init(&rb, storage, SMALL_CAPACITY) == 0);

	for (run = 0; run < 2; run++) {
		for (i = 0; i < 3; i++) {
			CHECK(ring_buffer_push(&rb, run * 3 + i) == 0);
		}
		CHECK(log_writer_start(&lw, &rb, "test", path) == 0);
		CHECK(log_writer_stop(&lw, NULL) == 0);
	}

	check_log_file(path, 6);

	ring_buffer_destroy(&rb);
	unlink(path);
}

int main(void)
{
	test_format_line_produces_exact_json();
	test_format_line_rejects_bad_input();
	test_start_rejects_bad_arguments();
	test_start_reports_unwritable_path();
	test_stop_drains_prefilled_buffer();
	test_concurrent_producer_loses_nothing();
	test_second_run_appends_to_existing_file();

	if (failures != 0) {
		printf("%d check(s) failed\n", failures);
		return 1;
	}
	printf("log_writer: all checks passed\n");

	return 0;
}
