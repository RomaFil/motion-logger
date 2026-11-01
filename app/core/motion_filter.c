/**
 * @file motion_filter.c
 * @brief Реалізація фільтра руху. Опис ідеї й правил дивись у motion_filter.h.
 */
#include "motion_filter.h"

#include <errno.h>
#include <math.h>

/* Приватні функції описано тут, а визначено внизу: публічні функції йдуть першими. */
static double compute_deviation(struct motion_filter *filter, double x, double y, double z);

int motion_filter_init(struct motion_filter *filter, size_t window_size, double threshold_g)
{
	if (filter == NULL || window_size == 0 || window_size > MOTION_FILTER_MAX_WINDOW) {
		return -EINVAL;
	}
	/* Порівняння з NaN завжди хибне, тому "не (>= 0)" відсікає і від'ємні, і NaN. */
	if (!(threshold_g >= 0.0)) {
		return -EINVAL;
	}

	filter->window_size = window_size;
	filter->threshold_g = threshold_g;
	filter->release_g = threshold_g;
	filter->moving = false;
	filter->next = 0;
	filter->count = 0;
	filter->baseline_g = 0.0;
	filter->baseline_ready = false;

	return 0;
}

int motion_filter_set_release(struct motion_filter *filter, double release_g)
{
	if (filter == NULL || !(release_g >= 0.0) || release_g > filter->threshold_g) {
		return -EINVAL;
	}
	filter->release_g = release_g;

	return 0;
}

int motion_filter_add_sample(struct motion_filter *filter, double x, double y, double z)
{
	double average;

	if (filter == NULL || !isfinite(x) || !isfinite(y) || !isfinite(z)) {
		return -EINVAL;
	}

	filter->deviations[filter->next] = compute_deviation(filter, x, y, z);
	filter->next = (filter->next + 1) % filter->window_size;
	if (filter->count < filter->window_size) {
		filter->count++;
	}

	if (filter->count < filter->window_size) {
		return 0; /* вікно ще неповне: руху нема */
	}
	average = motion_filter_get_average(filter);
	if (!filter->moving && average > filter->threshold_g) {
		filter->moving = true;
	} else if (filter->moving && average <= filter->release_g) {
		filter->moving = false;
	}

	return 0;
}

double motion_filter_get_average(const struct motion_filter *filter)
{
	double sum = 0.0;
	size_t i;

	if (filter->count == 0) {
		return 0.0;
	}

	/*
	 * Суму рахуємо заново, а не підтримуємо "плюс нове мінус старе": так дрібні похибки
	 * дробових чисел не накопичуються, а 10-64 додавання коштують дуже мало.
	 */
	for (i = 0; i < filter->count; i++) {
		sum += filter->deviations[i];
	}

	return sum / (double)filter->count;
}

bool motion_filter_is_moving(const struct motion_filter *filter) { return filter->moving; }

/*
 * Наскільки довжина вектора прискорення відрізняється від свого повільного середнього, g.
 *
 * Спершу міряємо відхилення від середнього ДО цього відліку, тому різкий удар видно повністю,
 * і лише потім зсуваємо середнє до нового відліку на 1/MOTION_FILTER_BASELINE_SAMPLES.
 * Перший відлік задає середнє, тож на старті відхилення нуль.
 */
static double compute_deviation(struct motion_filter *filter, double x, double y, double z)
{
	double length = sqrt(x * x + y * y + z * z);
	double deviation;

	if (!filter->baseline_ready) {
		filter->baseline_g = length;
		filter->baseline_ready = true;
		return 0.0;
	}

	deviation = fabs(length - filter->baseline_g);
	filter->baseline_g +=
	    (length - filter->baseline_g) / (double)MOTION_FILTER_BASELINE_SAMPLES;

	return deviation;
}
