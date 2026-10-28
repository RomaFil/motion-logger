/**
 * @file main.c
 * @brief Точка входу motion_logger: бере відліки з джерела, кладе в буфер, пише лог.
 *
 * Джерело обирається аргументом (рівно одне):
 *   --stdin           числа від імітатора зі стандартного вводу:
 *                       ./build/sensor_sim | ./build/motion_logger --stdin --log motion.jsonl
 *   --replay <файл>   «програвач» CSV-запису акселерометра (формат t_ms,x_g,y_g,z_g):
 *                       ./build/motion_logger --replay tests/data/accel_sample.csv --log
 * motion.jsonl До --replay можна додати --speed <число>: 1 реальний час (за замовчуванням), 50 у 50
 * разів швидше, 0 без пауз (тоді буфер може переповнитись, втрати видно в підсумку). Без --log
 * відліки лише збираються в буфері й у файл не пишуться. Без жодного джерела програма нічого не
 * читає й одразу завершується успішно: так її запускає скрипт автозапуску образу (там stdin це
 * /dev/null).
 *
 * Це єдине місце, яке знає і про ядро (core/), і про драйвер (drivers/csv_player.c):
 * ядро працює лише з інтерфейсом struct sensor.
 */
#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "csv_player.h"
#include "log_writer.h"
#include "ring_buffer.h"
#include "sample_reader.h"
#include "sensor_reader.h"

#define BUFFER_CAPACITY 4096
#define STDIN_OPTION "--stdin"
#define REPLAY_OPTION "--replay"
#define SPEED_OPTION "--speed"
#define LOG_OPTION "--log"

#define REALTIME_SPEED 1.0

/* Параметри запуску, розібрані з командного рядка. */
struct options {
	bool read_stdin;
	const char *replay_path; /* NULL, якщо --replay не задано */
	double replay_speed;
	const char *log_path; /* NULL, якщо --log не задано */
};

/* Скільки відліків прийшло від джерела і скільки з них не влізло в буфер. */
struct source_stats {
	size_t received;
	size_t dropped;
};

/* Стає 1, коли користувач натиснув Ctrl+C або init просить зупинитись (SIGTERM). */
static volatile sig_atomic_t stop_requested;

static int parse_options(int argc, char **argv, struct options *options);
static int parse_speed(const char *text, double *speed);
static void print_usage(const char *program);
static void handle_stop_signal(int signal_number);
static int install_stop_handlers(void);
static int run_source(const struct options *options, struct ring_buffer *buffer,
		      struct source_stats *stats);
static int run_replay(const struct options *options, struct ring_buffer *buffer,
		      struct source_stats *stats);
static int run_stdin(struct ring_buffer *buffer, struct source_stats *stats);

int main(int argc, char **argv)
{
	static struct sample storage[BUFFER_CAPACITY];
	struct ring_buffer buffer;
	struct source_stats source_stats = {0};
	struct log_writer_stats log_stats = {0};
	struct log_writer writer;
	struct options options;
	bool writer_started = false;
	int result;

	if (parse_options(argc, argv, &options) < 0) {
		print_usage(argv[0]);
		return EXIT_FAILURE;
	}
	if (!options.read_stdin && options.replay_path == NULL) {
		fprintf(stderr, "motion_logger: no sample source (use %s or %s)\n", STDIN_OPTION,
			REPLAY_OPTION);
		return EXIT_SUCCESS;
	}

	result = ring_buffer_init(&buffer, storage, BUFFER_CAPACITY);
	if (result < 0) {
		fprintf(stderr, "motion_logger: ring buffer: %s\n", strerror(-result));
		return EXIT_FAILURE;
	}

	result = install_stop_handlers();
	if (result < 0) {
		fprintf(stderr, "motion_logger: signals: %s\n", strerror(-result));
		goto cleanup;
	}

	if (options.log_path != NULL) {
		result = log_writer_start(&writer, &buffer, options.log_path);
		if (result < 0) {
			fprintf(stderr, "motion_logger: log %s: %s\n", options.log_path,
				strerror(-result));
			goto cleanup;
		}
		writer_started = true;
	}

	result = run_source(&options, &buffer, &source_stats);

	/* Джерело закінчило, нових записів не буде: тепер записувач може дописати залишок. */
	if (writer_started) {
		int stop_result = log_writer_stop(&writer, &log_stats);

		if (stop_result < 0) {
			fprintf(stderr, "motion_logger: log close: %s\n", strerror(-stop_result));
			result = result < 0 ? result : stop_result;
		}
	}

	printf(
	    "received %zu, dropped %zu, logged %zu (events %zu), log errors %zu, in buffer %zu\n",
	    source_stats.received, source_stats.dropped, log_stats.written, log_stats.events,
	    log_stats.failed, ring_buffer_count(&buffer));

cleanup:
	ring_buffer_destroy(&buffer);
	return result < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}

/*
 * Розбирає аргументи; повертає 0, або -EINVAL, якщо аргумент невідомий, бракує значення,
 * джерел два або --speed без --replay.
 */
static int parse_options(int argc, char **argv, struct options *options)
{
	bool speed_given = false;
	int i;

	options->read_stdin = false;
	options->replay_path = NULL;
	options->replay_speed = REALTIME_SPEED;
	options->log_path = NULL;

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], STDIN_OPTION) == 0) {
			options->read_stdin = true;
		} else if (strcmp(argv[i], REPLAY_OPTION) == 0 && i + 1 < argc) {
			options->replay_path = argv[++i];
		} else if (strcmp(argv[i], SPEED_OPTION) == 0 && i + 1 < argc) {
			if (parse_speed(argv[++i], &options->replay_speed) < 0) {
				return -EINVAL;
			}
			speed_given = true;
		} else if (strcmp(argv[i], LOG_OPTION) == 0 && i + 1 < argc) {
			options->log_path = argv[++i];
		} else {
			return -EINVAL;
		}
	}

	if (options->read_stdin && options->replay_path != NULL) {
		return -EINVAL; /* джерело одне */
	}
	if (speed_given && options->replay_path == NULL) {
		return -EINVAL; /* швидкість має зміст лише для --replay */
	}

	return 0;
}

/* Розбирає швидкість програвання: невід'ємне десяткове число, увесь текст. */
static int parse_speed(const char *text, double *speed)
{
	char *end;
	double value;

	errno = 0;
	value = strtod(text, &end);
	if (errno != 0 || end == text || *end != '\0' || !(value >= 0)) {
		return -EINVAL;
	}
	*speed = value;

	return 0;
}

static void print_usage(const char *program)
{
	fprintf(stderr, "usage: %s [%s | %s file [%s factor]] [%s file]\n", program, STDIN_OPTION,
		REPLAY_OPTION, SPEED_OPTION, LOG_OPTION);
}

/* У обробнику сигналу можна лише виставити прапорець: усе інше в ньому небезпечне. */
static void handle_stop_signal(int signal_number)
{
	(void)signal_number;
	stop_requested = 1;
}

static int install_stop_handlers(void)
{
	struct sigaction action = {.sa_handler = handle_stop_signal};

	sigemptyset(&action.sa_mask);
	if (sigaction(SIGINT, &action, NULL) < 0 || sigaction(SIGTERM, &action, NULL) < 0) {
		return -errno;
	}

	return 0;
}

/* Запускає обране джерело. */
static int run_source(const struct options *options, struct ring_buffer *buffer,
		      struct source_stats *stats)
{
	if (options->replay_path != NULL) {
		return run_replay(options, buffer, stats);
	}

	return run_stdin(buffer, stats);
}

static int run_stdin(struct ring_buffer *buffer, struct source_stats *stats)
{
	struct sample_reader_stats read_stats;
	int result = sample_reader_run(STDIN_FILENO, buffer, &stop_requested, &read_stats);

	stats->received = read_stats.received;
	stats->dropped = read_stats.dropped;
	if (result < 0) {
		fprintf(stderr, "motion_logger: reader: %s\n", strerror(-result));
	}

	return result;
}

/* Єдине місце, де драйвер програвача підставляється як «датчик» для ядра. */
static int run_replay(const struct options *options, struct ring_buffer *buffer,
		      struct source_stats *stats)
{
	struct sensor_reader_stats read_stats;
	struct csv_player player;
	int result = csv_player_open(&player, options->replay_path, options->replay_speed);

	if (result < 0) {
		fprintf(stderr, "motion_logger: replay %s: %s\n", options->replay_path,
			strerror(-result));
		return result;
	}

	result = sensor_reader_run(&player.base, buffer, &stop_requested, &read_stats);
	stats->received = read_stats.received;
	stats->dropped = read_stats.dropped;
	if (result < 0) {
		fprintf(stderr, "motion_logger: replay %s line %lu: %s\n", options->replay_path,
			player.line_number, strerror(-result));
	}
	player.base.close(&player.base);

	return result;
}
