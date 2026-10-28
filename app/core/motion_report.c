/**
 * @file motion_report.c
 * @brief Реалізація звіту руху по секундах. Опис ідеї дивись у motion_report.h.
 */
#include "motion_report.h"

#include <errno.h>
#include <string.h>

#define MSEC_PER_SEC 1000

/* Приватні функції описано тут, а визначено внизу: публічні функції йдуть першими. */
static void close_second(struct motion_report *report, struct motion_report_row *row);

int motion_report_init(struct motion_report *report, size_t window_size, double threshold_g,
		       size_t confirm_samples)
{
	int error;

	if (report == NULL) {
		return -EINVAL;
	}

	error = motion_filter_init(&report->filter, window_size, threshold_g);
	if (error < 0) {
		return error;
	}
	error = motion_events_init(&report->events, confirm_samples);
	if (error < 0) {
		return error;
	}

	report->has_second = false;
	report->current_second = 0;
	report->last_time_ms = 0;
	report->samples = 0;
	report->moving_samples = 0;
	report->deviation_sum = 0.0;

	return 0;
}

int motion_report_add_sample(struct motion_report *report, const struct accel_sample *sample,
			     struct motion_report_update *update)
{
	int64_t second;
	int error;

	if (report == NULL || sample == NULL || update == NULL) {
		return -EINVAL;
	}
	if (report->has_second && sample->time_ms < report->last_time_ms) {
		return -EINVAL;
	}

	/* Фільтр відхиляє нескінченні й нечислові значення, не змінюючи свого стану. */
	error = motion_filter_add_sample(&report->filter, sample->x_g, sample->y_g, sample->z_g);
	if (error < 0) {
		return error;
	}

	memset(update, 0, sizeof(*update));
	second = sample->time_ms / MSEC_PER_SEC;

	/* Відлік належить новій секунді: спершу закриваємо попередню. */
	if (report->has_second && second != report->current_second) {
		close_second(report, &update->row);
		update->row_ready = true;
	}
	report->has_second = true;
	report->current_second = second;
	report->last_time_ms = sample->time_ms;

	report->samples++;
	report->deviation_sum += motion_filter_get_average(&report->filter);
	if (motion_filter_is_moving(&report->filter)) {
		report->moving_samples++;
	}
	update->event =
	    motion_events_update(&report->events, motion_filter_is_moving(&report->filter));

	return 0;
}

int motion_report_finish(struct motion_report *report, struct motion_report_update *update)
{
	if (report == NULL || update == NULL) {
		return -EINVAL;
	}

	memset(update, 0, sizeof(*update));
	if (report->has_second && report->samples > 0) {
		close_second(report, &update->row);
		update->row_ready = true;
	}
	report->has_second = false;

	return 0;
}

/* Складає рядок звіту про відкриту секунду й обнуляє лічильники під наступну. */
static void close_second(struct motion_report *report, struct motion_report_row *row)
{
	row->second = report->current_second;
	row->samples = report->samples;
	row->moving_samples = report->moving_samples;
	row->moving = report->moving_samples * 2 > report->samples;
	row->average_deviation_g = report->deviation_sum / (double)report->samples;

	report->samples = 0;
	report->moving_samples = 0;
	report->deviation_sum = 0.0;
}
