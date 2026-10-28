/**
 * @file main.c
 * @brief Точка входу motion_logger: читає відліки з джерела, кладе в буфер, пише лог.
 *
 * Поки справжніх датчиків у програмі нема, джерелом служить стандартний ввід:
 *   ./build/sensor_sim | ./build/motion_logger --stdin --log motion.jsonl
 * Без --stdin програма нічого не читає й одразу завершується успішно: так її
 * запускає скрипт автозапуску образу (там stdin це /dev/null).
 * Без --log числа лише збираються в буфері й у файл не пишуться.
 */
#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "log_writer.h"
#include "ring_buffer.h"
#include "sample_reader.h"

#define BUFFER_CAPACITY 4096
#define STDIN_OPTION "--stdin"
#define LOG_OPTION "--log"

/* Ім'я джерела в кожному рядку логу; з'являться справжні датчики, і воно стане різним. */
#define SOURCE_NAME "sim"

/* Параметри запуску, розібрані з командного рядка. */
struct options {
	bool read_stdin;
	const char *log_path; /* NULL, якщо --log не задано */
};

/* Стає 1, коли користувач натиснув Ctrl+C або init просить зупинитись (SIGTERM). */
static volatile sig_atomic_t stop_requested;

static int parse_options(int argc, char **argv, struct options *options);
static void handle_stop_signal(int signal_number);
static int install_stop_handlers(void);

int main(int argc, char **argv)
{
	static int32_t storage[BUFFER_CAPACITY];
	struct ring_buffer buffer;
	struct sample_reader_stats read_stats;
	struct log_writer_stats log_stats = {0};
	struct log_writer writer;
	struct options options;
	bool writer_started = false;
	int result;

	if (parse_options(argc, argv, &options) < 0) {
		fprintf(stderr, "usage: %s [%s [%s file]]\n", argv[0], STDIN_OPTION, LOG_OPTION);
		return EXIT_FAILURE;
	}
	if (!options.read_stdin) {
		fprintf(stderr, "motion_logger: no sample source (use %s)\n", STDIN_OPTION);
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
		result = log_writer_start(&writer, &buffer, SOURCE_NAME, options.log_path);
		if (result < 0) {
			fprintf(stderr, "motion_logger: log %s: %s\n", options.log_path,
				strerror(-result));
			goto cleanup;
		}
		writer_started = true;
	}

	result = sample_reader_run(STDIN_FILENO, &buffer, &stop_requested, &read_stats);
	if (result < 0) {
		fprintf(stderr, "motion_logger: reader: %s\n", strerror(-result));
	}

	/* Читач закінчив, нових чисел не буде: тепер записувач може дописати залишок. */
	if (writer_started) {
		int stop_result = log_writer_stop(&writer, &log_stats);

		if (stop_result < 0) {
			fprintf(stderr, "motion_logger: log close: %s\n", strerror(-stop_result));
			result = result < 0 ? result : stop_result;
		}
	}

	printf("received %zu, dropped %zu, logged %zu, log errors %zu, in buffer %zu\n",
	       read_stats.received, read_stats.dropped, log_stats.written, log_stats.failed,
	       ring_buffer_count(&buffer));

cleanup:
	ring_buffer_destroy(&buffer);
	return result < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}

/* Розбирає аргументи; повертає 0, або -EINVAL, якщо аргумент невідомий чи бракує шляху. */
static int parse_options(int argc, char **argv, struct options *options)
{
	int i;

	options->read_stdin = false;
	options->log_path = NULL;

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], STDIN_OPTION) == 0) {
			options->read_stdin = true;
		} else if (strcmp(argv[i], LOG_OPTION) == 0 && i + 1 < argc) {
			options->log_path = argv[++i];
		} else {
			return -EINVAL;
		}
	}

	return 0;
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
