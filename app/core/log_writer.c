/**
 * @file log_writer.c
 * @brief Реалізація записувача логу. Опис формату й правил дивись у log_writer.h.
 */
#include "log_writer.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "io_util.h"
#include "sensor.h"

/* Як довго потік спить, коли в буфері порожньо. 10 мс = не більше ~100 прокидань на секунду. */
#define IDLE_SLEEP_MS 10

#define NSEC_PER_MSEC 1000000L

/* Ім'я джерела в рядках логу. Лише a-z, 0-9, _: воно йде в JSON без екранування. */
#define SIM_SOURCE_NAME "sim"

#define LOG_FILE_MODE 0644

/* Приватні функції описано тут, а визначено внизу: публічні функції йдуть першими. */
static bool is_valid_source(const char *source);
static void *run_writer_thread(void *argument);
static void handle_sample(struct log_writer *lw, const struct sample *item);
static void write_sim_sample(struct log_writer *lw, const struct sample *item);
static void write_line(struct log_writer *lw, const char *line, int length);
static void sleep_idle(void);

int log_writer_format_line(char *out, size_t size, const char *source, int64_t timestamp_ms,
			   int32_t value)
{
	int length;

	if (out == NULL || !is_valid_source(source)) {
		return -EINVAL;
	}

	length =
	    snprintf(out, size, "{\"t_ms\":%" PRId64 ",\"src\":\"%s\",\"value\":%" PRId32 "}\n",
		     timestamp_ms, source, value);
	if (length < 0 || (size_t)length >= size) {
		return -ENOSPC;
	}

	return length;
}

int log_writer_start(struct log_writer *lw, struct ring_buffer *rb, const char *path)
{
	int error;

	if (lw == NULL || rb == NULL || path == NULL) {
		return -EINVAL;
	}

	lw->fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, LOG_FILE_MODE);
	if (lw->fd < 0) {
		return -errno;
	}

	lw->source_buffer = rb;
	memset(&lw->stats, 0, sizeof(lw->stats));
	atomic_init(&lw->stop_requested, false);

	error = pthread_create(&lw->thread, NULL, run_writer_thread, lw);
	if (error != 0) {
		close(lw->fd);
		return -error;
	}

	return 0;
}

int log_writer_stop(struct log_writer *lw, struct log_writer_stats *stats)
{
	int result = 0;

	if (lw == NULL) {
		return -EINVAL;
	}

	atomic_store(&lw->stop_requested, true);
	pthread_join(lw->thread, NULL);

	/* Після join потік уже не пише, тож підсумок можна читати без замка. */
	if (stats != NULL) {
		*stats = lw->stats;
	}

	if (close(lw->fd) < 0) {
		result = -errno;
	}

	return result;
}

/* Ім'я джерела потрапляє в JSON без екранування, тому дозволяємо лише безпечні символи. */
static bool is_valid_source(const char *source)
{
	size_t length;
	size_t i;

	if (source == NULL) {
		return false;
	}

	length = strlen(source);
	if (length == 0 || length >= LOG_WRITER_SOURCE_SIZE) {
		return false;
	}

	for (i = 0; i < length; i++) {
		char c = source[i];

		if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) {
			return false;
		}
	}

	return true;
}

static void *run_writer_thread(void *argument)
{
	struct log_writer *lw = argument;
	struct sample item;

	for (;;) {
		/*
		 * Прапорець читаємо ДО спроби забрати запис. Якщо зупинку вже запросили, виробники
		 * закінчили, і порожній буфер означає, що справді все дописано. Якби читали після
		 * pop, запис, покладений між pop і перевіркою, лишився б у буфері.
		 */
		bool stopping = atomic_load(&lw->stop_requested);

		if (ring_buffer_pop(lw->source_buffer, &item) == 0) {
			handle_sample(lw, &item);
			continue;
		}
		if (stopping) {
			break;
		}
		sleep_idle();
	}

	return NULL;
}

/* Розподіляє запис за джерелом: число імітатора пишемо одразу, відлік акселерометра йде у фільтр.
 */
static void handle_sample(struct log_writer *lw, const struct sample *item)
{
	switch (item->source) {
	case SAMPLE_SOURCE_SIM:
		write_sim_sample(lw, item);
		break;
	default:
		lw->stats.failed++;
		break;
	}
}

/* Число імітатора: один рядок на число. */
static void write_sim_sample(struct log_writer *lw, const struct sample *item)
{
	char line[LOG_WRITER_LINE_SIZE];
	int length =
	    log_writer_format_line(line, sizeof(line), SIM_SOURCE_NAME, item->time_ms, item->value);

	write_line(lw, line, length);
}

/* Записує готовий рядок одним викликом (length < 0 це помилка форматування); рахує результат. */
static void write_line(struct log_writer *lw, const char *line, int length)
{
	if (length < 0 || write_all(lw->fd, line, (size_t)length) < 0) {
		lw->stats.failed++;
		return;
	}

	lw->stats.written++;
}

static void sleep_idle(void)
{
	struct timespec pause = {.tv_sec = 0, .tv_nsec = IDLE_SLEEP_MS * NSEC_PER_MSEC};

	nanosleep(&pause, NULL);
}
