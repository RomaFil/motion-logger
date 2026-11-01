/**
 * @file stage_indicator.h
 * @brief Показує світлодіодами етап руху: до першого руху, під час руху, після зупинки.
 *
 * Етап змінюють події фільтра руху (MOTION_EVENT_START і MOTION_EVENT_STOP), тому світлодіоди
 * збігаються з рядками MOVE_START / MOVE_STOP у логу. Якщо після зупинки рух почнеться знову,
 * індикатор знову покаже «рух». Працює через інтерфейс stage_io, тож не знає про GPIO.
 */
#ifndef STAGE_INDICATOR_H
#define STAGE_INDICATOR_H

#include "motion_events.h"
#include "stage_io.h"

/** @brief Стан індикатора. Поля змінюй лише через функції stage_indicator_*. */
struct stage_indicator {
	struct stage_io *io;
	enum stage current; /**< етап, який зараз горить */
};

/**
 * @brief Готує індикатор і одразу показує етап «до руху».
 *
 * @return 0; мінус код помилки світлодіода; -EINVAL, якщо si чи io NULL
 */
int stage_indicator_init(struct stage_indicator *si, struct stage_io *io);

/**
 * @brief Враховує подію руху: START дає етап «рух», STOP дає «після». MOTION_EVENT_NONE
 *        нічого не міняє. Світлодіоди чіпаються лише при зміні етапу.
 *
 * @return 0; мінус код помилки світлодіода; -EINVAL, якщо si NULL
 */
int stage_indicator_on_event(struct stage_indicator *si, enum motion_event event);

#endif /* STAGE_INDICATOR_H */
