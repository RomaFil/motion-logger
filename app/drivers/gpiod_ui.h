/**
 * @file gpiod_ui.h
 * @brief Кнопка й три світлодіоди етапів руху через libgpiod 1.x (інтерфейс ui_io).
 *
 * Лінії за замовчуванням збігаються з нашою платою (docs: «Плата Motion Logger — таблиця
 * з'єднань»): кнопка BTN на GPIO17; світлодіоди, як їх видно зліва направо: 1 на GPIO22
 * (до руху), 2 (середній) на GPIO6 (рух), 3 на GPIO27 (після зупинки). Порядок ліній світлодіодів
 * перевірено на платі 01.11.2026 по черзі ввімкнення. Номер мікросхеми залежить від ядра: у нашому
 * образі RP1 це gpiochip0 (дивись gpiodetect). Світлодіоди підключені анодом через резистор, тож
 * 1 означає «світить».
 *
 * Лінія GPIO6 після старту прошивки висить на підтяжці до живлення, і світлодіод 2 ледь тліє;
 * щойно драйвер замовляє лінію як вихід із нулем, тління зникає.
 *
 * Кнопка модуля KY-004 вважається активною високим рівнем (при натисканні сигнал стає 1), із
 * підтяжкою до землі; якщо на платі навпаки, `button_active_high = false`. Це припущення, не
 * перевірене на залізі. Без кнопки (`use_button = false`) wait_press просто чекає заданий час.
 *
 * Драйвер компілюється лише з GPIOD=1 (див. Makefile). Не потокобезпечний: ним користується
 * один потік контролера.
 */
#ifndef GPIOD_UI_H
#define GPIOD_UI_H

#include <stdbool.h>

#include "ui_io.h"

#define GPIOD_UI_DEFAULT_BUTTON_LINE 17
#define GPIOD_UI_DEFAULT_BEFORE_LINE 22
#define GPIOD_UI_DEFAULT_MOVING_LINE 6
#define GPIOD_UI_DEFAULT_AFTER_LINE 27

#define GPIOD_UI_LED_COUNT 3

/** @brief Опис підключення. */
struct gpiod_ui_config {
	const char *chip;	  /**< ім'я чи шлях: "gpiochip0", "/dev/gpiochip0" */
	bool use_button;	  /**< чи є кнопка */
	unsigned int button_line; /**< номер лінії кнопки */
	unsigned int before_line; /**< світлодіод 1: до руху */
	unsigned int moving_line; /**< світлодіод 2: рух */
	unsigned int after_line;  /**< світлодіод 3: після зупинки */
	bool button_active_high;  /**< true: натисканню відповідає зростання напруги */
};

/** @brief Структуру, що описує дефолтні лінії нашої плати, для мікросхеми chip. */
struct gpiod_ui_config gpiod_ui_default_config(const char *chip);

struct gpiod_chip;
struct gpiod_line;

/** @brief Стан драйвера. Поля змінюй лише через функції gpiod_ui_* і через base. */
struct gpiod_ui {
	struct ui_io base; /**< ПЕРШЕ поле: покажчик на нього це покажчик на драйвер */
	struct gpiod_chip *chip;
	struct gpiod_line *button;		    /**< або NULL, якщо кнопки нема */
	struct gpiod_line *led[GPIOD_UI_LED_COUNT]; /**< за номером етапу: до, рух, після */
};

/**
 * @brief Відкриває мікросхему, замовляє лінії, вимикає всі світлодіоди.
 *
 * Закрити: ui->base.close(&ui->base).
 *
 * @return 0 при успіху; мінус код помилки відкриття мікросхеми чи замовлення лінії;
 *         -EINVAL, якщо ui чи config NULL
 */
int gpiod_ui_open(struct gpiod_ui *ui, const struct gpiod_ui_config *config);

#endif /* GPIOD_UI_H */
