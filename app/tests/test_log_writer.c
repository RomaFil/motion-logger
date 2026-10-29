/**
 * @file test_log_writer.c
 * @brief Перевірки записувача логу: формат рядка, порядок, повнота, зупинка.
 *
 * Запуск: make test (звичайний) або make test-tsan (з перевіркою гонок даних).
 * Код виходу 0 означає, що всі перевірки пройшли. Лог-файли тест створює в /tmp
 * і видаляє сам.
 */
#include <errno.h>
#include <math.h>
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

/* Рядок-підсумок секунди: розбирає час, номер секунди й ознаку руху; false, якщо не схожий. */
static bool parse_row_line(const char *line, long long *time_ms, long long *second, bool *moving)
{
	char flag[8];
	double deviation;

	if (sscanf(line,
		   "{\"t_ms\":%lld,\"src\":\"accel\",\"second\":%lld,\"moving\":%7[a-z],"
		   "\"avg_dev_g\":%lf}",
		   time_ms, second, flag, &deviation) != 4) {
		return false;
	}
	*moving = strcmp(flag, "true") == 0;

	return *moving || strcmp(flag, "false") == 0;
}

/*
 * Читає лог і перевіряє: серед рядків рівно дві події (MOVE_START у межах руху, потім MOVE_STOP
 * після нього), решта це підсумки секунд з номерами по порядку; під час руху є рядок moving:true.
 * Повертає кількість рядків-підсумків.
 */
static size_t check_start_then_stop(const char *path, int64_t moving_start_ms,
				    int64_t moving_end_ms)
{
	char line[LOG_WRITER_LINE_SIZE];
	char name[LOG_WRITER_SOURCE_SIZE];
	long long time_ms;
	long long second;
	long long previous_second = -1;
	long long start_ms = -1;
	long long stop_ms = -1;
	bool moving;
	bool saw_moving_row = false;
	size_t rows = 0;
	size_t events = 0;
	FILE *file = fopen(path, "r");

	CHECK(file != NULL);
	if (file == NULL) {
		return 0;
	}

	while (fgets(line, sizeof(line), file) != NULL) {
		if (parse_event_line(line, &time_ms, name, sizeof(name))) {
			if (strcmp(name, "MOVE_START") == 0) {
				CHECK(events == 0);
				start_ms = time_ms;
			} else {
				CHECK(strcmp(name, "MOVE_STOP") == 0);
				CHECK(events == 1);
				stop_ms = time_ms;
			}
			events++;
		} else {
			CHECK(parse_row_line(line, &time_ms, &second, &moving));
			CHECK(second == previous_second + 1);
			previous_second = second;
			saw_moving_row = saw_moving_row || moving;
			rows++;
		}
	}
	fclose(file);

	CHECK(events == 2);
	CHECK(start_ms > moving_start_ms && start_ms < moving_end_ms);
	CHECK(stop_ms > moving_end_ms);
	CHECK(saw_moving_row);

	return rows;
}

/*
 * Головна перевірка кроку 5.3: «стою, іду, стою» дає в лозі рівно MOVE_START і MOVE_STOP, а між
 * ними підсумки секунд. Остання секунда ще не закрита, тому підсумків на один менше за секунди.
 */
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
	size_t rows;
	char path[TEMP_PATH_SIZE];

	make_temp_path(path);
	CHECK(ring_buffer_init(&rb, storage, sizeof(storage) / sizeof(storage[0])) == 0);
	samples = fill_still_moving_still(&rb, &moving_start_ms, &moving_end_ms);
	CHECK(samples == sizeof(storage) / sizeof(storage[0]));

	CHECK(log_writer_start(&lw, &rb, path) == 0);
	CHECK(log_writer_stop(&lw, &stats) == 0);

	CHECK(stats.events == 2);
	CHECK(stats.rows == STILL_SECONDS + MOVING_SECONDS + STILL_SECONDS - 1);
	CHECK(stats.written == stats.events + stats.rows);
	CHECK(stats.failed == 0);
	CHECK(ring_buffer_is_empty(&rb));
	rows = check_start_then_stop(path, moving_start_ms, moving_end_ms);
	CHECK(rows == stats.rows);

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

static void test_format_row_produces_exact_json(void)
{
	char line[LOG_WRITER_LINE_SIZE];
	struct motion_report_row row = {.second = 20,
					.samples = 100,
					.moving_samples = 90,
					.moving = true,
					.average_deviation_g = 0.21051};

	CHECK(log_writer_format_row(line, sizeof(line), 21000, &row) > 0);
	CHECK(strcmp(line, "{\"t_ms\":21000,\"src\":\"accel\",\"second\":20,\"moving\":true,"
			   "\"avg_dev_g\":0.2105}\n") == 0);

	row.moving = false;
	row.average_deviation_g = 0.0;
	CHECK(log_writer_format_row(line, sizeof(line), 3000, &row) > 0);
	CHECK(strcmp(line, "{\"t_ms\":3000,\"src\":\"accel\",\"second\":20,\"moving\":false,"
			   "\"avg_dev_g\":0.0000}\n") == 0);
}

static void test_format_row_rejects_bad_input(void)
{
	char line[LOG_WRITER_LINE_SIZE];
	char tiny[8];
	struct motion_report_row row = {.second = 1, .average_deviation_g = 0.1};

	CHECK(log_writer_format_row(NULL, sizeof(line), 0, &row) == -EINVAL);
	CHECK(log_writer_format_row(line, sizeof(line), 0, NULL) == -EINVAL);
	CHECK(log_writer_format_row(tiny, sizeof(tiny), 0, &row) == -ENOSPC);
	row.average_deviation_g = NAN;
	CHECK(log_writer_format_row(line, sizeof(line), 0, &row) == -EINVAL);
}

static void test_format_aux_produces_exact_json(void)
{
	char line[LOG_WRITER_LINE_SIZE];

	CHECK(log_writer_format_aux(line, sizeof(line), 21000, 0.0123, -0.0042, 0.9987) > 0);
	CHECK(strcmp(line, "{\"t_ms\":21000,\"src\":\"accel2\",\"x_g\":0.0123,\"y_g\":-0.0042,"
			   "\"z_g\":0.9987}\n") == 0);
}

static void test_format_aux_rejects_bad_input(void)
{
	char line[LOG_WRITER_LINE_SIZE];
	char tiny[8];

	CHECK(log_writer_format_aux(NULL, sizeof(line), 0, 0, 0, 1) == -EINVAL);
	CHECK(log_writer_format_aux(tiny, sizeof(tiny), 0, 0, 0, 1) == -ENOSPC);
	CHECK(log_writer_format_aux(line, sizeof(line), 0, NAN, 0, 1) == -EINVAL);
	CHECK(log_writer_format_aux(line, sizeof(line), 0, 0, INFINITY, 1) == -EINVAL);
	CHECK(log_writer_format_aux(line, sizeof(line), 0, 0, 0, -INFINITY) == -EINVAL);
}

static struct sample make_gps_item(int64_t time_ms, bool fix, int satellites)
{
	struct sample item = {.source = SAMPLE_SOURCE_GPS,
			      .time_ms = time_ms,
			      .gps_fix = fix,
			      .satellites = satellites};

	if (fix) {
		item.latitude_deg = 50.4501;
		item.longitude_deg = 30.5234;
		item.speed_knots = 0.3;
	}

	return item;
}

static void test_format_gps_produces_exact_json(void)
{
	char line[LOG_WRITER_LINE_SIZE];
	struct sample item = make_gps_item(21000, true, 8);

	CHECK(log_writer_format_gps(line, sizeof(line), &item) > 0);
	CHECK(strcmp(line, "{\"t_ms\":21000,\"src\":\"gps\",\"fix\":true,\"lat\":50.450100,"
			   "\"lon\":30.523400,\"speed_knots\":0.30,\"sats\":8}\n") == 0);

	/* Південь і захід: мінус зберігається. */
	item.latitude_deg = -33.8688;
	item.longitude_deg = -70.6693;
	CHECK(log_writer_format_gps(line, sizeof(line), &item) > 0);
	CHECK(strstr(line, "\"lat\":-33.868800,\"lon\":-70.669300") != NULL);
}

/* Додаткові поля йдуть після основних і лише відомі; без UTC чи висоти їх у рядку нема. */
static void test_format_gps_with_optional_fields(void)
{
	char line[LOG_WRITER_LINE_SIZE];
	struct sample item = make_gps_item(21000, true, 8);

	item.has_utc = true;
	item.utc_epoch_s = 1793188808; /* 2026-10-28T12:00:08Z */
	item.has_altitude = true;
	item.altitude_m = 180.04;
	item.has_course = true;
	item.course_deg = 90.0;
	item.has_hdop = true;
	item.hdop = 1.2;
	CHECK(log_writer_format_gps(line, sizeof(line), &item) > 0);
	CHECK(strcmp(line, "{\"t_ms\":21000,\"src\":\"gps\",\"fix\":true,\"lat\":50.450100,"
			   "\"lon\":30.523400,\"speed_knots\":0.30,\"sats\":8,"
			   "\"utc\":\"2026-10-28T12:00:08Z\",\"alt_m\":180.0,\"course_deg\":90.0,"
			   "\"hdop\":1.20}\n") == 0);

	/* Лише висота: решта необов'язкових полів не з'являється. */
	item.has_utc = false;
	item.has_course = false;
	item.has_hdop = false;
	item.altitude_m = -12.5;
	CHECK(log_writer_format_gps(line, sizeof(line), &item) > 0);
	CHECK(strstr(line, "\"sats\":8,\"alt_m\":-12.5}\n") != NULL);
	CHECK(strstr(line, "utc") == NULL && strstr(line, "hdop") == NULL);
}

/* Найдовший можливий рядок GPS вміщається в LOG_WRITER_LINE_SIZE, а в малий буфер не лізе. */
static void test_format_gps_longest_line_fits(void)
{
	char line[LOG_WRITER_LINE_SIZE];
	char tiny[64];
	struct sample item = make_gps_item(INT64_C(9999999999999), true, 12);

	item.latitude_deg = -89.999999;
	item.longitude_deg = -179.999999;
	item.speed_knots = 999.99;
	item.has_utc = true;
	item.utc_epoch_s = 4102444799LL;
	item.has_altitude = true;
	item.altitude_m = -9999.9;
	item.has_course = true;
	item.course_deg = 359.9;
	item.has_hdop = true;
	item.hdop = 99.99;

	CHECK(log_writer_format_gps(line, sizeof(line), &item) > 0);
	CHECK(log_writer_format_gps(tiny, sizeof(tiny), &item) == -ENOSPC);

	item.altitude_m = NAN;
	CHECK(log_writer_format_gps(line, sizeof(line), &item) == -EINVAL);
}

/* Без фіксації координат у рядку нема зовсім; невідома кількість супутників це null, а не 0. */
static void test_format_gps_without_fix_has_no_coordinates(void)
{
	char line[LOG_WRITER_LINE_SIZE];
	struct sample item = make_gps_item(3000, false, 0);

	CHECK(log_writer_format_gps(line, sizeof(line), &item) > 0);
	CHECK(strcmp(line, "{\"t_ms\":3000,\"src\":\"gps\",\"fix\":false,\"sats\":0}\n") == 0);

	item.satellites = -1;
	CHECK(log_writer_format_gps(line, sizeof(line), &item) > 0);
	CHECK(strcmp(line, "{\"t_ms\":3000,\"src\":\"gps\",\"fix\":false,\"sats\":null}\n") == 0);
	CHECK(strstr(line, "lat") == NULL);
}

static void test_format_gps_rejects_bad_input(void)
{
	char line[LOG_WRITER_LINE_SIZE];
	char tiny[8];
	struct sample item = make_gps_item(0, true, 5);
	struct sample not_gps = make_sim_sample(1);

	CHECK(log_writer_format_gps(NULL, sizeof(line), &item) == -EINVAL);
	CHECK(log_writer_format_gps(line, sizeof(line), NULL) == -EINVAL);
	CHECK(log_writer_format_gps(line, sizeof(line), &not_gps) == -EINVAL);
	CHECK(log_writer_format_gps(tiny, sizeof(tiny), &item) == -ENOSPC);
	item.latitude_deg = NAN;
	CHECK(log_writer_format_gps(line, sizeof(line), &item) == -EINVAL);
}

/* Додатковий акселерометр на 100 Гц дає в лог не більше рядка на секунду, перший відлік одразу. */
static void test_aux_samples_are_written_once_per_second(void)
{
	struct sample storage[ACCEL_RATE_HZ * 3];
	struct ring_buffer rb;
	struct log_writer lw;
	struct log_writer_stats stats;
	char path[TEMP_PATH_SIZE];
	int64_t time_ms;

	make_temp_path(path);
	CHECK(ring_buffer_init(&rb, storage, sizeof(storage) / sizeof(storage[0])) == 0);
	for (time_ms = 0; time_ms < 2500; time_ms += MS_PER_SAMPLE) {
		struct sample item = {
		    .source = SAMPLE_SOURCE_ACCEL2, .time_ms = time_ms, .z_g = 1.0};

		CHECK(ring_buffer_push(&rb, &item) == 0);
	}

	CHECK(log_writer_start(&lw, &rb, path) == 0);
	CHECK(log_writer_stop(&lw, &stats) == 0);

	CHECK(stats.aux == 3); /* t = 0, 1000, 2000 */
	CHECK(stats.written == 3);
	CHECK(stats.failed == 0);

	ring_buffer_destroy(&rb);
	unlink(path);
}

/* Кожен запис GPS іде в лог окремим рядком у тому порядку, в якому прийшов. */
static void test_gps_samples_are_written_in_order(void)
{
	struct sample storage[SMALL_CAPACITY];
	struct ring_buffer rb;
	struct log_writer lw;
	struct log_writer_stats stats;
	struct sample item;
	char path[TEMP_PATH_SIZE];
	char line[LOG_WRITER_LINE_SIZE];
	FILE *file;

	make_temp_path(path);
	CHECK(ring_buffer_init(&rb, storage, SMALL_CAPACITY) == 0);
	item = make_gps_item(1000, false, -1);
	CHECK(ring_buffer_push(&rb, &item) == 0);
	item = make_gps_item(2000, true, 7);
	CHECK(ring_buffer_push(&rb, &item) == 0);

	CHECK(log_writer_start(&lw, &rb, path) == 0);
	CHECK(log_writer_stop(&lw, &stats) == 0);

	CHECK(stats.gps == 2);
	CHECK(stats.written == 2);
	file = fopen(path, "r");
	CHECK(file != NULL);
	if (file != NULL) {
		CHECK(fgets(line, sizeof(line), file) != NULL);
		CHECK(strstr(line, "\"t_ms\":1000,\"src\":\"gps\",\"fix\":false") != NULL);
		CHECK(fgets(line, sizeof(line), file) != NULL);
		CHECK(strstr(line, "\"t_ms\":2000,\"src\":\"gps\",\"fix\":true") != NULL);
		CHECK(fgets(line, sizeof(line), file) == NULL);
		fclose(file);
	}

	ring_buffer_destroy(&rb);
	unlink(path);
}

/* Запис з невідомим джерелом не губиться мовчки: його видно в лічильнику помилок. */
static void test_unknown_source_counts_as_failed(void)
{
	struct sample storage[SMALL_CAPACITY];
	struct ring_buffer rb;
	struct log_writer lw;
	struct log_writer_stats stats;
	struct sample item = make_sim_sample(1);
	char path[TEMP_PATH_SIZE];

	item.source = (enum sample_source)99;
	make_temp_path(path);
	CHECK(ring_buffer_init(&rb, storage, SMALL_CAPACITY) == 0);
	CHECK(ring_buffer_push(&rb, &item) == 0);

	CHECK(log_writer_start(&lw, &rb, path) == 0);
	CHECK(log_writer_stop(&lw, &stats) == 0);

	CHECK(stats.failed == 1);
	CHECK(stats.written == 0);

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
	test_format_row_produces_exact_json();
	test_format_row_rejects_bad_input();
	test_format_aux_produces_exact_json();
	test_format_aux_rejects_bad_input();
	test_format_gps_produces_exact_json();
	test_format_gps_without_fix_has_no_coordinates();
	test_format_gps_with_optional_fields();
	test_format_gps_longest_line_fits();
	test_format_gps_rejects_bad_input();
	test_aux_samples_are_written_once_per_second();
	test_gps_samples_are_written_in_order();
	test_unknown_source_counts_as_failed();

	if (failures != 0) {
		printf("%d check(s) failed\n", failures);
		return 1;
	}
	printf("log_writer: all checks passed\n");

	return 0;
}
