/**
 * @file sensor_reader.c
 * @brief Реалізація читача датчика. Опис і правила дивись у sensor_reader.h.
 */
#include "sensor_reader.h"

#include <errno.h>
#include <string.h>

/* Пакує відлік акселерометра в запис для буфера. */
static struct sample make_accel_sample(const struct accel_sample *accel, enum sample_source source);

int sensor_reader_run(struct sensor *sensor, enum sample_source source, struct ring_buffer *rb,
		      const volatile sig_atomic_t *stop, struct sensor_reader_stats *stats)
{
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
		if (result < 0) {
			return result;
		}

		item = make_accel_sample(&accel, source);
		stats->received++;
		if (ring_buffer_push(rb, &item) != 0) {
			stats->dropped++;
		}
	}

	return 0;
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
