/**
 * @file csv_player.h
 * @brief Драйвер-«програвач»: віддає відліки акселерометра із CSV-файлу так, ніби це датчик.
 *
 * Файл у форматі accel_csv (t_ms,x_g,y_g,z_g). Програвач реалізує інтерфейс struct sensor
 * (interfaces/sensor.h), тож решта програми не відрізняє його від справжнього датчика.
 * Це дає змогу відпрацювати весь конвеєр (буфер, фільтр, лог) на запису, коли плати нема,
 * і повторити на ноутбуці те, що сталося на столі.
 *
 * Швидкість: speed = 1 це реальний час (відлік віддається, коли від початку програвання
 * мине його t_ms); speed = 50 це у 50 разів швидше; speed = 0 без пауз, як швидко вдасться.
 * Пауза рахується від початку програвання (абсолютні моменти), тому затримка кожного
 * відліку не накопичується.
 *
 * Модуль не потокобезпечний: користується ним один потік.
 */
#ifndef CSV_PLAYER_H
#define CSV_PLAYER_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "sensor.h"

/** @brief Стан програвача. Поля змінюй лише через функції csv_player_*. */
struct csv_player {
	struct sensor base; /**< ПЕРШЕ поле: покажчик на нього це покажчик на програвач */
	FILE *file;
	double speed;
	bool started;
	int64_t start_ms; /**< момент монотонного годинника, якому відповідає t_ms = 0 */
	bool has_pending; /**< відлік уже прочитано з файлу, але пауза перед ним перервана */
	struct accel_sample pending;
	unsigned long line_number; /**< останній прочитаний рядок (для повідомлень про помилки) */
};

/**
 * @brief Відкриває файл і готує програвач.
 *
 * Закрити: player->base.close(&player->base).
 *
 * @param player програвач, який треба підготувати; після успіху &player->base це датчик
 * @param path   шлях до CSV-файлу
 * @param speed  швидкість програвання (див. вище); не може бути від'ємною чи NaN
 * @return 0 при успіху; -EINVAL, якщо player чи path NULL або speed недопустима; інакше
 *         мінус код помилки відкриття файлу
 */
int csv_player_open(struct csv_player *player, const char *path, double speed);

#endif /* CSV_PLAYER_H */
