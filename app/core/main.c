/**
 * @file main.c
 * @brief Точка входу motion_logger: читає відліки з джерела й складає їх у кільцевий буфер.
 *
 * Поки справжніх датчиків у програмі нема, джерелом служить стандартний ввід:
 *   ./build/sensor_sim | ./build/motion_logger --stdin
 * Без аргументів програма нічого не читає й одразу завершується успішно: так її
 * запускає скрипт автозапуску образу (там stdin це /dev/null).
 */
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ring_buffer.h"
#include "sample_reader.h"

#define BUFFER_CAPACITY 4096
#define STDIN_OPTION "--stdin"

/* Стає 1, коли користувач натиснув Ctrl+C або init просить зупинитись (SIGTERM). */
static volatile sig_atomic_t stop_requested;

static void handle_stop_signal(int signal_number);
static int install_stop_handlers(void);

int main(int argc, char **argv)
{
	static int32_t storage[BUFFER_CAPACITY];
	struct ring_buffer buffer;
	struct sample_reader_stats stats;
	int result;

	if (argc != 2 || strcmp(argv[1], STDIN_OPTION) != 0) {
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
		ring_buffer_destroy(&buffer);
		return EXIT_FAILURE;
	}

	result = sample_reader_run(STDIN_FILENO, &buffer, &stop_requested, &stats);
	if (result < 0) {
		fprintf(stderr, "motion_logger: reader: %s\n", strerror(-result));
	}

	printf("received %zu, dropped %zu, in buffer %zu\n", stats.received, stats.dropped,
	       ring_buffer_count(&buffer));

	ring_buffer_destroy(&buffer);
	return result < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
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
