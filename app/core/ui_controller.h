/**
 * @file ui_controller.h
 * @brief Цикл «кнопка й світлодіод»: чекає кнопку, перемикає запис, показує стан світлодіодами.
 *
 * Контролер працює через інтерфейс ui_io (кнопка, світлодіод) і набір функцій-гачків (запис,
 * рух), тому нічого не знає ні про GPIO, ні про log_writer: їх підставляє main.c. Гачки
 * викликаються з потоку контролера.
 */
#ifndef UI_CONTROLLER_H
#define UI_CONTROLLER_H

#include <signal.h>
#include <stdbool.h>
#include <stdint.h>

#include "recording_ui.h"
#include "ui_io.h"

/** @brief Як контролер керує рештою програми. */
struct ui_hooks {
	void (*set_recording)(void *context,
			      bool recording); /**< кнопку прийнято: вкл/викл запис */
	bool (*is_moving)(void *context);      /**< чи зараз зафіксовано рух */
	void *context;
};

/** @brief Стан контролера. Поля змінюй лише через функції ui_controller_*. */
struct ui_controller {
	struct recording_ui ui;
	struct ui_io *io;
	struct ui_hooks hooks;
	enum ui_view shown; /**< що зараз показано */
	bool shown_valid;   /**< чи вже щось показували */
};

/** @brief Готує контролер; нічого не показує, поки не буде першого кроку. */
void ui_controller_init(struct ui_controller *ctl, struct ui_io *io, const struct ui_hooks *hooks);

/**
 * @brief Один крок: обробляє натискання (якщо було), питає про рух, міняє показ, якщо треба.
 *
 * @param pressed чи натиснули кнопку з попереднього кроку
 * @param now_ms  поточний час, мілісекунди монотонного годинника
 * @return 0; мінус код помилки світлодіода
 */
int ui_controller_step(struct ui_controller *ctl, bool pressed, int64_t now_ms);

/**
 * @brief Виконує кроки, доки не з'явиться прохання зупинитись або не станеться помилка.
 *
 * @param ctl  контролер після ui_controller_init
 * @param stop прапорець зупинки (перевіряється принаймні раз на UI_CONTROLLER_WAIT_MS)
 * @return 0 при зупинці за проханням; мінус код помилки кнопки чи світлодіода
 */
int ui_controller_run(struct ui_controller *ctl, const volatile sig_atomic_t *stop);

/** @brief Скільки мілісекунд контролер чекає кнопку за один раз (світлодіоди оновлюються з цим
 * кроком).
 */
#define UI_CONTROLLER_WAIT_MS 100

#endif /* UI_CONTROLLER_H */
