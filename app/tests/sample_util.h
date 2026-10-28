/**
 * @file sample_util.h
 * @brief Дрібні помічники тестів: швидко зібрати struct sample і покласти її в буфер.
 */
#ifndef SAMPLE_UTIL_H
#define SAMPLE_UTIL_H

#include <stdint.h>

#include "ring_buffer.h"
#include "sensor.h"

/* Запис імітатора з числом value і нульовим часом. */
static inline struct sample make_sim_sample(int32_t value)
{
	struct sample item = {.source = SAMPLE_SOURCE_SIM, .time_ms = 0, .value = value};

	return item;
}

/* Запис акселерометра. */
static inline struct sample make_accel_item(int64_t time_ms, double x_g, double y_g, double z_g)
{
	struct sample item = {
	    .source = SAMPLE_SOURCE_ACCEL, .time_ms = time_ms, .x_g = x_g, .y_g = y_g, .z_g = z_g};

	return item;
}

/* Кладе в буфер запис імітатора з числом value; результат такий, як у ring_buffer_push. */
static inline int push_value(struct ring_buffer *rb, int32_t value)
{
	struct sample item = make_sim_sample(value);

	return ring_buffer_push(rb, &item);
}

#endif /* SAMPLE_UTIL_H */
