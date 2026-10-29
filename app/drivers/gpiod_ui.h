/**
 * @file gpiod_ui.h
 * @brief Кнопка й RGB-світлодіод через libgpiod 1.x (інтерфейс ui_io).
 *
 * Лінії за замовчуванням збігаються з нашою платою (docs: «Плата Motion Logger — таблиця
 * з'єднань»): кнопка BTN на GPIO17, світлодіод R на GPIO27, G на GPIO22, B на GPIO6. Номер
 * мікросхеми (chip) на Pi 5 залежить від ядра: GPIO виведені через RP1, дивись gpiodetect на платі.
 *
 * Кнопка модуля KY-004 вважається активною високим рівнем (при натисканні сигнал стає 1), із
 * підтяжкою до землі; якщо на платі навпаки, `button_active_high = false`. Це припущення, не
 * перевірене на залізі. Світлодіоди підключені анодом через резистор, тож 1 означає «світить».
 *
 * Драйвер компілюється лише з GPIOD=1 (див. Makefile). Не потокобезпечний: ним користується
 * один потік контролера.
 */
#ifndef GPIOD_UI_H
#define GPIOD_UI_H

#include <stdbool.h>

#include "ui_io.h"

#define GPIOD_UI_DEFAULT_BUTTON_LINE 17
#define GPIOD_UI_DEFAULT_RED_LINE 27
#define GPIOD_UI_DEFAULT_GREEN_LINE 22
#define GPIOD_UI_DEFAULT_BLUE_LINE 6

/** @brief Опис підключення. */
struct gpiod_ui_config {
	const char *chip;	  /**< ім'я чи шлях: "gpiochip4", "/dev/gpiochip4" */
	unsigned int button_line; /**< номер лінії кнопки */
	unsigned int red_line;
	unsigned int green_line;
	unsigned int blue_line;
	bool button_active_high; /**< true: натисканню відповідає зростання напруги */
};

/** @brief Структуру, що описує дефолтні лінії нашої плати, для мікросхеми chip. */
struct gpiod_ui_config gpiod_ui_default_config(const char *chip);

struct gpiod_chip;
struct gpiod_line;

/** @brief Стан драйвера. Поля змінюй лише через функції gpiod_ui_* і через base. */
struct gpiod_ui {
	struct ui_io base; /**< ПЕРШЕ поле: покажчик на нього це покажчик на драйвер */
	struct gpiod_chip *chip;
	struct gpiod_line *button;
	struct gpiod_line *led[3]; /**< червоний, зелений, синій */
};

/**
 * @brief Відкриває мікросхему, замовляє лінії, вимикає світлодіод.
 *
 * Закрити: ui->base.close(&ui->base).
 *
 * @return 0 при успіху; мінус код помилки відкриття мікросхеми чи замовлення лінії;
 *         -EINVAL, якщо ui чи config NULL
 */
int gpiod_ui_open(struct gpiod_ui *ui, const struct gpiod_ui_config *config);

#endif /* GPIOD_UI_H */
