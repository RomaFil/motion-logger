/**
 * @file recording_ui.h
 * @brief Стан «записуємо чи пауза» й етап руху для світлодіодів. Чиста логіка без заліза й часу.
 *
 * Кнопка перемикає запис: пауза ↔ запис. Світлодіоди показують (enum ui_view):
 *   пауза (запис вимкнено): горять усі три;
 *   записуємо, руху ще не було: світлодіод 1;
 *   записуємо, плата рухається: світлодіод 2;
 *   записуємо, рух закінчився: світлодіод 3.
 * Після паузи запис і фільтр руху починають заново (log_writer), тому етап знову «до руху».
 * Натискання ближче ніж RECORDING_UI_DEBOUNCE_MS до попереднього прийнятого вважається
 * дрижанням контактів і ігнорується. Час передає викликач, тож логіку легко перевіряти тестом.
 */
#ifndef RECORDING_UI_H
#define RECORDING_UI_H

#include <stdbool.h>
#include <stdint.h>

#include "ui_io.h"

/** @brief Скільки мілісекунд після прийнятого натискання нові натискання ігноруються. */
#define RECORDING_UI_DEBOUNCE_MS 200

/** @brief Стан інтерфейсу. Поля змінюй лише через функції recording_ui_*. */
struct recording_ui {
	bool recording;
	bool moving;
	bool motion_seen;      /**< чи був рух після початку (чи відновлення) запису */
	bool press_seen;       /**< чи було вже прийняте натискання */
	int64_t last_press_ms; /**< час останнього прийнятого натискання */
};

/** @brief Початковий стан: запис іде, руху ще не було. */
void recording_ui_init(struct recording_ui *ui);

/**
 * @brief Обробляє натискання кнопки.
 * @param now_ms час натискання, мілісекунди монотонного годинника
 * @return true, якщо натискання прийнято і запис перемкнуто; false, якщо це дрижання контактів
 *         чи час пішов назад
 */
bool recording_ui_press(struct recording_ui *ui, int64_t now_ms);

/** @brief Запам'ятовує, чи є рух (береться з log_writer_is_moving). */
void recording_ui_set_moving(struct recording_ui *ui, bool moving);

/** @brief Що показувати зараз. */
enum ui_view recording_ui_view(const struct recording_ui *ui);

#endif /* RECORDING_UI_H */
