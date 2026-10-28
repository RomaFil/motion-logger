/**
 * @file sample_reader.c
 * @brief Реалізація читача відліків. Опис і правила дивись у sample_reader.h.
 */
#include "sample_reader.h"

#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <sys/epoll.h>
#include <unistd.h>

#include "io_util.h"

/* Як часто прокидаємось перевірити прапорець зупинки, якщо даних нема. */
#define POLL_TIMEOUT_MS 100

/* Скільки відліків читаємо за один виклик read(). */
#define CHUNK_SAMPLES 64

#define SAMPLE_SIZE sizeof(int32_t)
#define CHUNK_BYTES (CHUNK_SAMPLES * SAMPLE_SIZE)

/* Результат одного кроку читання (додатні значення; помилки це мінус errno). */
#define STEP_MORE_DATA_EXPECTED 0
#define STEP_SOURCE_CLOSED 1

/*
 * Канал може віддати відлік частинами: read() має право повернути 6 байтів, тобто
 * одне ціле число і половину другого. Недочитану половину зберігаємо тут до наступного кроку.
 */
struct partial_sample {
	uint8_t bytes[SAMPLE_SIZE];
	size_t length;
};

/* Приватні функції описано тут, а визначено внизу: публічна функція йде першою. */
static int register_source(int epoll_fd, int source_fd);
static int read_available(int source_fd, struct ring_buffer *rb, struct partial_sample *partial,
			  struct sample_reader_stats *stats);
static void store_sample(struct ring_buffer *rb, int32_t value, struct sample_reader_stats *stats);

int sample_reader_run(int source_fd, struct ring_buffer *rb, const volatile sig_atomic_t *stop,
		      struct sample_reader_stats *stats)
{
	struct partial_sample partial = {.length = 0};
	struct epoll_event event;
	int epoll_fd;
	int result = 0;

	if (source_fd < 0 || rb == NULL || stop == NULL || stats == NULL) {
		return -EINVAL;
	}
	memset(stats, 0, sizeof(*stats));

	epoll_fd = epoll_create1(EPOLL_CLOEXEC);
	if (epoll_fd < 0) {
		return -errno;
	}

	result = register_source(epoll_fd, source_fd);
	if (result < 0) {
		goto cleanup;
	}

	while (!*stop) {
		int ready = epoll_wait(epoll_fd, &event, 1, POLL_TIMEOUT_MS);

		if (ready < 0) {
			if (errno == EINTR) {
				/* Сигнал перервав чекання: на початку циклу перечитаємо прапорець.
				 */
				continue;
			}
			result = -errno;
			goto cleanup;
		}
		if (ready == 0) {
			continue;
		}

		result = read_available(source_fd, rb, &partial, stats);
		if (result < 0) {
			goto cleanup;
		}
		if (result == STEP_SOURCE_CLOSED) {
			result = 0;
			goto cleanup;
		}
	}

cleanup:
	close(epoll_fd);
	return result;
}

/* Просить epoll повідомляти, коли в джерелі з'являються дані для читання. */
static int register_source(int epoll_fd, int source_fd)
{
	struct epoll_event event = {.events = EPOLLIN, .data.fd = source_fd};

	if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, source_fd, &event) < 0) {
		return -errno;
	}

	return 0;
}

/*
 * Один виклик read(): забирає все, що зараз є (до CHUNK_SAMPLES відліків), розкладає на
 * числа й кладе в буфер. Повертає STEP_*, або мінус errno при помилці читання.
 */
static int read_available(int source_fd, struct ring_buffer *rb, struct partial_sample *partial,
			  struct sample_reader_stats *stats)
{
	uint8_t chunk[CHUNK_BYTES];
	ssize_t bytes_read;
	size_t total;
	size_t whole_samples;
	size_t offset;

	/* Недочитаний хвіст з попереднього кроку стає початком нового шматка. */
	memcpy(chunk, partial->bytes, partial->length);

	bytes_read = read(source_fd, chunk + partial->length, sizeof(chunk) - partial->length);
	if (bytes_read < 0) {
		if (errno == EINTR || errno == EAGAIN) {
			return STEP_MORE_DATA_EXPECTED;
		}
		return -errno;
	}
	if (bytes_read == 0) {
		return STEP_SOURCE_CLOSED;
	}

	total = partial->length + (size_t)bytes_read;
	whole_samples = total / SAMPLE_SIZE;

	for (offset = 0; offset < whole_samples * SAMPLE_SIZE; offset += SAMPLE_SIZE) {
		int32_t value;

		/* memcpy замість приведення покажчика: chunk не обов'язково вирівняний під int32_t.
		 */
		memcpy(&value, chunk + offset, SAMPLE_SIZE);
		store_sample(rb, value, stats);
	}

	partial->length = total - whole_samples * SAMPLE_SIZE;
	memcpy(partial->bytes, chunk + whole_samples * SAMPLE_SIZE, partial->length);

	return STEP_MORE_DATA_EXPECTED;
}

/* Пакує число в запис із міткою часу й кладе в буфер; якщо місця нема, лише рахує втрату. */
static void store_sample(struct ring_buffer *rb, int32_t value, struct sample_reader_stats *stats)
{
	struct sample item = {
	    .source = SAMPLE_SOURCE_SIM, .time_ms = monotonic_ms(), .value = value};

	stats->received++;
	if (ring_buffer_push(rb, &item) != 0) {
		stats->dropped++;
	}
}
