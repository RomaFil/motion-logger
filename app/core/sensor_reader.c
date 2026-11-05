/**
 * @file sensor_reader.c
 * @brief Реалізація читача датчика. Опис і правила дивись у sensor_reader.h.
 */
#include "sensor_reader.h"

#include <errno.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>

/* Пауза після тимчасової помилки: датчик, що віддає помилку миттєво, не крутить процесор. */
#define ERROR_PAUSE_NS 1000000L

/* Пакує відлік акселерометра в запис для буфера. */
static struct sample make_accel_sample(const struct accel_sample *accel, enum sample_source source);
static bool is_transient_error(int error);
static void pause_after_error(void);

int sensor_reader_run(struct sensor *sensor, enum sample_source source, struct ring_buffer *rb,
		      const volatile sig_atomic_t *stop, struct sensor_reader_stats *stats)
{
	size_t consecutive_errors = 0;

	if (sensor == NULL || sensor->read == NULL || rb == NULL || stop == NULL || stats == NULL) {
		return -EINVAL;
	}
	memset(stats, 0, sizeof(*stats));

	while (!*stop) {
		struct accel_sample accel = {0}; /* драйвер може не заповнити необов'язкові поля */
		struct sample item;
		int result = sensor->read(sensor, &accel);

		if (result == -EINTR) {
			continue; /* сигнал перервав очікування: на початку циклу перечитаємо
				     прапорець */
		}
		if (result == -ENODATA) {
			return 0;
		}
		if (result < 0 && is_transient_error(result)) {
			stats->errors++;
			consecutive_errors++;
			if (consecutive_errors >= SENSOR_READER_MAX_CONSECUTIVE_ERRORS) {
				return result;
			}
			pause_after_error();
			continue;
		}
		if (result < 0) {
			return result;
		}
		consecutive_errors = 0;

		item = make_accel_sample(&accel, source);
		stats->received++;
		if (ring_buffer_push(rb, &item) != 0) {
			stats->dropped++;
		}
	}

	return 0;
}

/* Помилки шини, після яких наступне читання цілком може вдатися. */
static bool is_transient_error(int error)
{
	return error == -EIO || error == -EREMOTEIO || error == -ETIMEDOUT || error == -EAGAIN ||
	       error == -EBUSY;
}

static void pause_after_error(void)
{
	struct timespec pause = {.tv_sec = 0, .tv_nsec = ERROR_PAUSE_NS};

	nanosleep(&pause, NULL);
}

static struct sample make_accel_sample(const struct accel_sample *accel, enum sample_source source)
{
	struct sample item = {.source = source,
			      .time_ms = accel->time_ms,
			      .x_g = accel->x_g,
			      .y_g = accel->y_g,
			      .z_g = accel->z_g,
			      .has_temperature = accel->has_temperature,
			      .temperature_c = accel->temperature_c};

	return item;
}
