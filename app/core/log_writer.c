/**
 * @file log_writer.c
 * @brief Реалізація записувача логу. Опис формату й правил дивись у log_writer.h.
 */
#include "log_writer.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "io_util.h"
#include "motion_defaults.h"
#include "sensor.h"

/* Як довго потік спить, коли в буфері порожньо. 10 мс = не більше ~100 прокидань на секунду. */
#define IDLE_SLEEP_MS 10

#define NSEC_PER_MSEC 1000000L

/* Ім'я джерела в рядках логу. Лише a-z, 0-9, _: воно йде в JSON без екранування. */
#define SIM_SOURCE_NAME "sim"
#define ACCEL_SOURCE_NAME "accel"
#define ACCEL2_SOURCE_NAME "accel2"
#define GPS_SOURCE_NAME "gps"

/* Додатковий акселерометр пишемо не частіше за цей інтервал: 100 відліків на секунду в лог не
 * потрібні. */
#define AUX_PERIOD_MS 1000

#define LOG_FILE_MODE 0644

/* Приватні функції описано тут, а визначено внизу: публічні функції йдуть першими. */
static bool is_valid_source(const char *source);
static void *run_writer_thread(void *argument);
static void handle_sample(struct log_writer *lw, const struct sample *item);
static void write_sim_sample(struct log_writer *lw, const struct sample *item);
static void write_accel_sample(struct log_writer *lw, const struct sample *item);
static void write_aux_sample(struct log_writer *lw, const struct sample *item);
static void write_gps_sample(struct log_writer *lw, const struct sample *item);
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

int log_writer_format_event(char *out, size_t size, int64_t timestamp_ms, enum motion_event event)
{
	const char *name;
	int length;

	if (out == NULL) {
		return -EINVAL;
	}
	if (event == MOTION_EVENT_START) {
		name = "MOVE_START";
	} else if (event == MOTION_EVENT_STOP) {
		name = "MOVE_STOP";
	} else {
		return -EINVAL;
	}

	length = snprintf(out, size, "{\"t_ms\":%" PRId64 ",\"src\":\"%s\",\"event\":\"%s\"}\n",
			  timestamp_ms, ACCEL_SOURCE_NAME, name);
	if (length < 0 || (size_t)length >= size) {
		return -ENOSPC;
	}

	return length;
}

int log_writer_format_row(char *out, size_t size, int64_t timestamp_ms,
			  const struct motion_report_row *row)
{
	int length;

	if (out == NULL || row == NULL || !isfinite(row->average_deviation_g)) {
		return -EINVAL;
	}

	length = snprintf(out, size,
			  "{\"t_ms\":%" PRId64 ",\"src\":\"%s\",\"second\":%" PRId64
			  ",\"moving\":%s,\"avg_dev_g\":%.4f}\n",
			  timestamp_ms, ACCEL_SOURCE_NAME, row->second,
			  row->moving ? "true" : "false", row->average_deviation_g);
	if (length < 0 || (size_t)length >= size) {
		return -ENOSPC;
	}

	return length;
}

int log_writer_format_aux(char *out, size_t size, int64_t timestamp_ms, double x_g, double y_g,
			  double z_g)
{
	int length;

	if (out == NULL || !isfinite(x_g) || !isfinite(y_g) || !isfinite(z_g)) {
		return -EINVAL;
	}

	length = snprintf(out, size,
			  "{\"t_ms\":%" PRId64
			  ",\"src\":\"%s\",\"x_g\":%.4f,\"y_g\":%.4f,\"z_g\":%.4f}\n",
			  timestamp_ms, ACCEL2_SOURCE_NAME, x_g, y_g, z_g);
	if (length < 0 || (size_t)length >= size) {
		return -ENOSPC;
	}

	return length;
}

int log_writer_format_gps(char *out, size_t size, const struct sample *sample)
{
	char sats[16];
	int length;

	if (out == NULL || sample == NULL || sample->source != SAMPLE_SOURCE_GPS) {
		return -EINVAL;
	}
	if (sample->satellites < 0) {
		snprintf(sats, sizeof(sats), "null");
	} else {
		snprintf(sats, sizeof(sats), "%d", sample->satellites);
	}

	if (!sample->gps_fix) {
		length = snprintf(
		    out, size, "{\"t_ms\":%" PRId64 ",\"src\":\"%s\",\"fix\":false,\"sats\":%s}\n",
		    sample->time_ms, GPS_SOURCE_NAME, sats);
	} else if (!isfinite(sample->latitude_deg) || !isfinite(sample->longitude_deg) ||
		   !isfinite(sample->speed_knots)) {
		return -EINVAL;
	} else {
		length = snprintf(out, size,
				  "{\"t_ms\":%" PRId64
				  ",\"src\":\"%s\",\"fix\":true,\"lat\":%.6f,\"lon\":%.6f,"
				  "\"speed_knots\":%.2f,\"sats\":%s}\n",
				  sample->time_ms, GPS_SOURCE_NAME, sample->latitude_deg,
				  sample->longitude_deg, sample->speed_knots, sats);
	}
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

	/* Параметри допустимі за побудовою (див. motion_defaults.h), помилка тут це помилка в них.
	 */
	error = motion_report_init(&lw->detector, MOTION_DEFAULT_WINDOW, MOTION_DEFAULT_THRESHOLD_G,
				   MOTION_DEFAULT_CONFIRM);
	if (error < 0) {
		return error;
	}

	lw->fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, LOG_FILE_MODE);
	if (lw->fd < 0) {
		return -errno;
	}

	lw->source_buffer = rb;
	lw->aux_seen = false;
	lw->last_aux_ms = 0;
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
	case SAMPLE_SOURCE_ACCEL:
		write_accel_sample(lw, item);
		break;
	case SAMPLE_SOURCE_ACCEL2:
		write_aux_sample(lw, item);
		break;
	case SAMPLE_SOURCE_GPS:
		write_gps_sample(lw, item);
		break;
	default:
		lw->stats.failed++; /* невідоме джерело: краще порахувати, ніж мовчки загубити */
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

/* Відлік основного акселерометра: у лог іде підсумок секунди й подія «рух почався / закінчився»,
 * сам відлік ні. */
static void write_accel_sample(struct log_writer *lw, const struct sample *item)
{
	struct accel_sample accel = {
	    .time_ms = item->time_ms, .x_g = item->x_g, .y_g = item->y_g, .z_g = item->z_g};
	struct motion_report_update update;
	char line[LOG_WRITER_LINE_SIZE];
	int length;

	if (motion_report_add_sample(&lw->detector, &accel, &update) < 0) {
		lw->stats.failed++; /* час спадає або число не скінченне: відлік відкинуто */
		return;
	}
	if (update.row_ready) {
		length = log_writer_format_row(line, sizeof(line), item->time_ms, &update.row);
		write_line(lw, line, length);
		if (length >= 0) {
			lw->stats.rows++;
		}
	}
	if (update.event == MOTION_EVENT_NONE) {
		return;
	}

	length = log_writer_format_event(line, sizeof(line), item->time_ms, update.event);
	write_line(lw, line, length);
	if (length >= 0) {
		lw->stats.events++;
	}
}

/* Додатковий акселерометр: не частіше одного рядка на AUX_PERIOD_MS за мітками часу відліків. */
static void write_aux_sample(struct log_writer *lw, const struct sample *item)
{
	char line[LOG_WRITER_LINE_SIZE];
	int length;

	if (lw->aux_seen && item->time_ms - lw->last_aux_ms < AUX_PERIOD_MS) {
		return;
	}

	length = log_writer_format_aux(line, sizeof(line), item->time_ms, item->x_g, item->y_g,
				       item->z_g);
	write_line(lw, line, length);
	if (length >= 0) {
		lw->stats.aux++;
		lw->aux_seen = true;
		lw->last_aux_ms = item->time_ms;
	}
}

/* GPS: один рядок на кожен рядок RMC від модуля. */
static void write_gps_sample(struct log_writer *lw, const struct sample *item)
{
	char line[LOG_WRITER_LINE_SIZE];
	int length = log_writer_format_gps(line, sizeof(line), item);

	write_line(lw, line, length);
	if (length >= 0) {
		lw->stats.gps++;
	}
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
