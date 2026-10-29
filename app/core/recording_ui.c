/**
 * @file recording_ui.c
 * @brief Реалізація стану кнопки й світлодіода. Опис і правила дивись у recording_ui.h.
 */
#include "recording_ui.h"

void recording_ui_init(struct recording_ui *ui)
{
	ui->recording = true;
	ui->moving = false;
	ui->press_seen = false;
	ui->last_press_ms = 0;
}

bool recording_ui_press(struct recording_ui *ui, int64_t now_ms)
{
	if (ui->press_seen) {
		int64_t since_last_ms = now_ms - ui->last_press_ms;

		/* Від'ємне значення: годинник пішов назад, таке натискання не можна вважати новим.
		 */
		if (since_last_ms < RECORDING_UI_DEBOUNCE_MS) {
			return false;
		}
	}

	ui->press_seen = true;
	ui->last_press_ms = now_ms;
	ui->recording = !ui->recording;

	return true;
}

void recording_ui_set_moving(struct recording_ui *ui, bool moving) { ui->moving = moving; }

enum ui_color recording_ui_color(const struct recording_ui *ui)
{
	if (!ui->recording) {
		return UI_COLOR_BLUE;
	}

	return ui->moving ? UI_COLOR_RED : UI_COLOR_GREEN;
}
