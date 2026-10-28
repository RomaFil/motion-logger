/**
 * @file motion_events.h
 * @brief Перетворює потік відповідей "рух є / руху нема" на події "рух почався / закінчився".
 *
 * Фільтр руху (motion_filter) відповідає на питання "рухається зараз?" після КОЖНОГО
 * відліку. Для журналу потрібні не відповіді, а ПОДІЇ: `MOVE START` і `MOVE STOP`.
 * Просте порівняння з попередньою відповіддю дало б десятки подій на секунду, бо під
 * час ходьби середнє відхилення блукає біля порога. Тому зміна стану підтверджується:
 * нова відповідь має повторитись confirm_samples РАЗІВ ПІДРЯД, і лише тоді стан змінюється
 * і з'являється подія. Одиночний збій у відповідях лише скидає лічильник підтвердження.
 *
 * Цей підхід називають "debounce" (усунення брязкоту), як у кнопки.
 * Модуль не потокобезпечний.
 */
#ifndef MOTION_EVENTS_H
#define MOTION_EVENTS_H

#include <stdbool.h>
#include <stddef.h>

/** @brief Подія, яку повертає motion_events_update(). */
enum motion_event {
	MOTION_EVENT_NONE,  /**< стан не змінився */
	MOTION_EVENT_START, /**< рух почався */
	MOTION_EVENT_STOP,  /**< рух закінчився */
};

/** @brief Стан відстеження подій. Поля змінюй лише через функції motion_events_*. */
struct motion_events {
	bool moving;		/**< підтверджений стан: рух є чи ні */
	size_t confirm_samples; /**< скільки однакових відповідей підряд змінюють стан */
	size_t streak; /**< скільки відповідей підряд уже суперечать підтвердженому стану */
};

/**
 * @brief Готує відстеження: початковий стан "руху нема".
 *
 * @param events          що ініціалізувати
 * @param confirm_samples скільки відповідей підряд підтверджують зміну (1 = без затримки)
 * @return 0 при успіху; -EINVAL, якщо events NULL або confirm_samples дорівнює нулю
 */
int motion_events_init(struct motion_events *events, size_t confirm_samples);

/**
 * @brief Враховує чергову відповідь фільтра й каже, чи змінився підтверджений стан.
 *
 * @param events     відстеження, ініціалізоване через motion_events_init()
 * @param moving_now поточна відповідь фільтра
 * @return MOTION_EVENT_START або MOTION_EVENT_STOP у момент зміни стану, інакше MOTION_EVENT_NONE
 */
enum motion_event motion_events_update(struct motion_events *events, bool moving_now);

#endif /* MOTION_EVENTS_H */
