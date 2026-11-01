/**
 * @file gpiod_stage.h
 * @brief Три світлодіоди етапів руху через libgpiod 1.x (інтерфейс stage_io).
 *
 * Лінії за замовчуванням збігаються з нашою платою, якою її видно зліва направо: світлодіод 1
 * (до руху) на GPIO22, світлодіод 2 (рух, середній) на GPIO6, світлодіод 3 (після) на GPIO27.
 * Це перевірено на платі 01.11.2026 по черзі ввімкнення ліній. Номер мікросхеми залежить від
 * ядра: у нашому образі RP1 це gpiochip0 (дивись gpiodetect). Світлодіоди підключені анодом через
 * резистор, тож 1 означає «світить».
 *
 * Лінія GPIO6 після старту прошивки висить на підтяжці до живлення, і світлодіод 2 ледь тліє;
 * щойно драйвер замовляє лінію як вихід із нулем, тління зникає.
 *
 * Драйвер компілюється лише з GPIOD=1 (див. Makefile). Не потокобезпечний: ним користується
 * один потік (потік записувача логу).
 */
#ifndef GPIOD_STAGE_H
#define GPIOD_STAGE_H

#include "stage_io.h"

#define GPIOD_STAGE_DEFAULT_BEFORE_LINE 22
#define GPIOD_STAGE_DEFAULT_MOVING_LINE 6
#define GPIOD_STAGE_DEFAULT_AFTER_LINE 27

#define GPIOD_STAGE_COUNT 3

struct gpiod_chip;
struct gpiod_line;

/** @brief Стан драйвера. Поля змінюй лише через функції gpiod_stage_* і через base. */
struct gpiod_stage {
	struct stage_io base; /**< ПЕРШЕ поле: покажчик на нього це покажчик на драйвер */
	struct gpiod_chip *chip;
	struct gpiod_line *led[GPIOD_STAGE_COUNT]; /**< за номером етапу: до, рух, після */
};

/**
 * @brief Відкриває мікросхему, замовляє три лінії як виходи й вимикає всі світлодіоди.
 *
 * Закрити: stage->base.close(&stage->base).
 *
 * @param chip ім'я чи шлях мікросхеми: "gpiochip0", "/dev/gpiochip0"
 * @return 0 при успіху; мінус код помилки відкриття мікросхеми чи замовлення лінії;
 *         -EINVAL, якщо stage чи chip NULL
 */
int gpiod_stage_open(struct gpiod_stage *stage, const char *chip);

#endif /* GPIOD_STAGE_H */
