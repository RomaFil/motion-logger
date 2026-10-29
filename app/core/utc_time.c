/**
 * @file utc_time.c
 * @brief Реалізація перетворень дати UTC. Опис дивись у utc_time.h.
 *
 * Алгоритми days_from_civil і civil_from_days Говарда Гіннанта (Howard Hinnant, «chrono-Compatible
 * Low-Level Date Algorithms»): рахують від 1 березня, тож високосний день стоїть в кінці року.
 */
#include "utc_time.h"

#include <stdio.h>

#define SECONDS_PER_DAY 86400
#define SECONDS_PER_HOUR 3600
#define SECONDS_PER_MINUTE 60
#define MAX_EPOCH 253402300799LL /* 9999-12-31T23:59:59Z */

/* Днів від 1970-01-01 до заданої дати. */
static int64_t days_from_civil(int64_t year, int month, int day)
{
	int64_t shifted_year = month <= 2 ? year - 1 : year;
	int64_t era = shifted_year / 400; /* для років >= 0 ділення вниз збігається зі звичайним */
	int64_t year_of_era = shifted_year - era * 400;
	int64_t day_of_year = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
	int64_t day_of_era = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;

	return era * 146097 + day_of_era - 719468;
}

/* Дата за кількістю днів від 1970-01-01. */
static void civil_from_days(int64_t days, int *year, int *month, int *day)
{
	int64_t shifted = days + 719468;
	int64_t era = shifted / 146097;
	int64_t day_of_era = shifted - era * 146097;
	int64_t year_of_era =
	    (day_of_era - day_of_era / 1460 + day_of_era / 36524 - day_of_era / 146096) / 365;
	int64_t day_of_year =
	    day_of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
	int64_t month_index = (5 * day_of_year + 2) / 153;

	*day = (int)(day_of_year - (153 * month_index + 2) / 5 + 1);
	*month = (int)(month_index < 10 ? month_index + 3 : month_index - 9);
	*year = (int)(year_of_era + era * 400 + (*month <= 2 ? 1 : 0));
}

int64_t utc_time_to_epoch(int year, int month, int day, uint32_t seconds_of_day)
{
	return days_from_civil(year, month, day) * SECONDS_PER_DAY + (int64_t)seconds_of_day;
}

int utc_time_format(char *out, size_t size, int64_t epoch)
{
	int64_t days;
	int64_t rest;
	int year;
	int month;
	int day;

	if (out == NULL || size < UTC_TIME_TEXT_SIZE || epoch < 0 || epoch > MAX_EPOCH) {
		return -1;
	}

	days = epoch / SECONDS_PER_DAY;
	rest = epoch % SECONDS_PER_DAY;
	civil_from_days(days, &year, &month, &day);

	return snprintf(out, size, "%04d-%02d-%02dT%02d:%02d:%02dZ", year, month, day,
			(int)(rest / SECONDS_PER_HOUR),
			(int)(rest % SECONDS_PER_HOUR / SECONDS_PER_MINUTE),
			(int)(rest % SECONDS_PER_MINUTE));
}
