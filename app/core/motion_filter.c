/**
 * @file motion_filter.c
 * @brief Реалізація фільтра руху. Опис ідеї й правил дивись у motion_filter.h.
 */
#include "motion_filter.h"

#include <errno.h>
#include <math.h>

/* Сила тяжіння в одиницях g, яку показує нерухомий акселерометр. */
#define GRAVITY_G 1.0

/* Приватні функції описано тут, а визначено внизу: публічні функції йдуть першими. */
static double compute_deviation(double x, double y, double z);

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
	filter->next = 0;
	filter->count = 0;

	return 0;
}

int motion_filter_add_sample(struct motion_filter *filter, double x, double y, double z)
{
	if (filter == NULL || !isfinite(x) || !isfinite(y) || !isfinite(z)) {
		return -EINVAL;
	}

	filter->deviations[filter->next] = compute_deviation(x, y, z);
	filter->next = (filter->next + 1) % filter->window_size;
	if (filter->count < filter->window_size) {
		filter->count++;
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

bool motion_filter_is_moving(const struct motion_filter *filter)
{
	if (filter->count < filter->window_size) {
		return false;
	}

	return motion_filter_get_average(filter) > filter->threshold_g;
}

/* Наскільки довжина вектора прискорення відрізняється від сили тяжіння, g. */
static double compute_deviation(double x, double y, double z)
{
	return fabs(sqrt(x * x + y * y + z * z) - GRAVITY_G);
}
