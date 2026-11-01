/**
 * @file motion_report.h
 * @brief Прогоняє записані відліки акселерометра через фільтр руху й видає звіт по секундах.
 *
 * Це "склейка" для кроку 3.5: фільтр руху (motion_filter) + події (motion_events) +
 * підрахунок по секундах. Модуль не читає файлів і не знає про залізо: йому подають відліки
 * один за одним (з файлу, з імітатора чи з датчика), а він повідомляє, коли
 *   - закінчилась секунда: рядок звіту "рух: так/ні" для неї;
 *   - змінився підтверджений стан: подія "рух почався / закінчився".
 *
 * Секунда вважається "рухомою", якщо фільтр відповідав "рух є" більш ніж на половині її
 * відліків. Секунди без жодного відліку у звіті відсутні. Час відліків не повинен спадати.
 */
#ifndef MOTION_REPORT_H
#define MOTION_REPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "accel_csv.h"
#include "motion_events.h"
#include "motion_filter.h"

/** @brief Підсумок однієї секунди запису. */
struct motion_report_row {
	int64_t second;		    /**< номер секунди від початку запису */
	size_t samples;		    /**< скільки відліків було в цій секунді */
	size_t moving_samples;	    /**< на скількох з них фільтр казав "рух є" */
	bool moving;		    /**< більше половини відліків рухомі */
	double average_deviation_g; /**< середнє відхилення за відліками секунди, g */
};

/** @brief Результат обробки одного відліку. */
struct motion_report_update {
	bool row_ready;		      /**< чи закрилась попередня секунда (тоді заповнено row) */
	struct motion_report_row row; /**< звіт про секунду, що щойно закрилась */
	enum motion_event event;      /**< подія, яку спричинив саме цей відлік */
};

/** @brief Стан звіту. Поля змінюй лише через функції motion_report_*. */
struct motion_report {
	struct motion_filter filter;
	struct motion_events events;
	bool has_second;
	int64_t current_second;
	int64_t last_time_ms;
	size_t samples;
	size_t moving_samples;
	double deviation_sum;
};

/**
 * @brief Готує звіт: розмір вікна й поріг фільтра, підтвердження зміни стану.
 *
 * @param report          що ініціалізувати
 * @param window_size     розмір вікна фільтра (див. motion_filter_init)
 * @param threshold_g     поріг руху, g
 * @param confirm_samples скільки відповідей підряд підтверджують зміну стану (див.
 * motion_events_init)
 * @return 0 при успіху; -EINVAL, якщо report NULL або будь-який параметр недопустимий
 */
int motion_report_init(struct motion_report *report, size_t window_size, double threshold_g,
		       size_t confirm_samples);

/**
 * @brief Задає поріг виходу з руху нижчий за поріг входу (див. motion_filter_set_release).
 *
 * Викликай після motion_report_init() і до перших відліків.
 *
 * @return 0 при успіху; -EINVAL, якщо report NULL або поріг недопустимий
 */
int motion_report_set_release(struct motion_report *report, double release_g);

/**
 * @brief Обробляє черговий відлік.
 *
 * @param report  звіт, ініціалізований через motion_report_init()
 * @param sample  відлік; його time_ms не менше за time_ms попереднього
 * @param update  сюди пишеться, чи закрилась секунда і яка подія сталась
 * @return 0 при успіху; -EINVAL, якщо вказівник NULL, час спадає або значення не скінченні;
 *         у такому разі стан звіту не змінюється
 */
int motion_report_add_sample(struct motion_report *report, const struct accel_sample *sample,
			     struct motion_report_update *update);

/**
 * @brief Закриває останню неповну секунду після кінця запису.
 *
 * @param report звіт
 * @param update row_ready стає true, якщо ще була відкрита секунда; event завжди NONE
 * @return 0 при успіху; -EINVAL, якщо вказівник NULL
 */
int motion_report_finish(struct motion_report *report, struct motion_report_update *update);

#endif /* MOTION_REPORT_H */
