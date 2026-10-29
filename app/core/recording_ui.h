/**
 * @file recording_ui.h
 * @brief Стан «записуємо чи пауза» і колір світлодіода. Чиста логіка без заліза й без часу.
 *
 * Кнопка перемикає запис: пауза ↔ запис. Колір показує стан:
 *   синій   пауза (запис вимкнено);
 *   зелений записуємо, плата стоїть;
 *   червоний записуємо, плата рухається.
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
	bool press_seen;       /**< чи було вже прийняте натискання */
	int64_t last_press_ms; /**< час останнього прийнятого натискання */
};

/** @brief Початковий стан: запис іде, плата вважається нерухомою. */
void recording_ui_init(struct recording_ui *ui);

/**
 * @brief Обробляє натискання кнопки.
 *
 * @param now_ms час натискання, мілісекунди монотонного годинника
 * @return true, якщо натискання прийнято і запис перемкнуто; false, якщо це дрижання контактів
 *         чи час пішов назад
 */
bool recording_ui_press(struct recording_ui *ui, int64_t now_ms);

/** @brief Запам'ятовує, чи є рух (береться з log_writer_is_moving). */
void recording_ui_set_moving(struct recording_ui *ui, bool moving);

/** @brief Колір світлодіода для поточного стану. */
enum ui_color recording_ui_color(const struct recording_ui *ui);

#endif /* RECORDING_UI_H */
