/**
 * @file sensor_sim.c
 * @brief Імітатор датчика: пише в stdout числа 0, 1, 2, ... (int32_t) раз на 10 мс.
 *
 * Використання:
 *   sensor_sim            пише без кінця (зупинка Ctrl+C)
 *   sensor_sim <кількість> пише задану кількість чисел і завершується
 *
 * Типове застосування (канал shell з'єднує імітатор із застосунком):
 *   ./build/sensor_sim | ./build/motion_logger --stdin
 *
 * Числа йдуть по порядку, щоб читач міг перевірити, що нічого не загубилось.
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "io_util.h"

#define SAMPLE_PERIOD_MS 10
#define NSEC_PER_MSEC 1000000L
#define NSEC_PER_SEC 1000000000L

/* Додає до моменту часу період відліку. Абсолютні моменти не накопичують затримку циклу. */
static void advance_deadline(struct timespec *deadline);

/* Розбирає аргумент "кількість"; повертає -1 при помилці, інакше кількість (0 означає "без кінця").
 */
static long parse_count(const char *text);

int main(int argc, char **argv)
{
	struct timespec deadline;
	long limit = 0;
	long sent;

	if (argc > 2) {
		fprintf(stderr, "usage: %s [count]\n", argv[0]);
		return EXIT_FAILURE;
	}
	if (argc == 2) {
		limit = parse_count(argv[1]);
		if (limit < 0) {
			fprintf(stderr, "%s: count must be a non-negative integer\n", argv[0]);
			return EXIT_FAILURE;
		}
	}

	clock_gettime(CLOCK_MONOTONIC, &deadline);

	for (sent = 0; limit == 0 || sent < limit; sent++) {
		int32_t value = (int32_t)sent;
		int result = write_all(STDOUT_FILENO, &value, sizeof(value));

		if (result < 0) {
			fprintf(stderr, "sensor_sim: write: %s\n", strerror(-result));
			return EXIT_FAILURE;
		}

		advance_deadline(&deadline);
		while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline, NULL) == EINTR) {
			/* Сигнал розбудив нас раніше часу: просто спимо далі до того ж моменту. */
		}
	}

	return EXIT_SUCCESS;
}

static void advance_deadline(struct timespec *deadline)
{
	deadline->tv_nsec += SAMPLE_PERIOD_MS * NSEC_PER_MSEC;
	if (deadline->tv_nsec >= NSEC_PER_SEC) {
		deadline->tv_nsec -= NSEC_PER_SEC;
		deadline->tv_sec++;
	}
}

static long parse_count(const char *text)
{
	char *end;
	long value;

	errno = 0;
	value = strtol(text, &end, 10);
	if (errno != 0 || end == text || *end != '\0' || value < 0) {
		return -1;
	}

	return value;
}
