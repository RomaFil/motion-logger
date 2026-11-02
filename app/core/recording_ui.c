/**
 * @file recording_ui.c
 * @brief Реалізація стану кнопки й світлодіодів. Опис і правила дивись у recording_ui.h.
 */
#include "recording_ui.h"

void recording_ui_init(struct recording_ui *ui)
{
	ui->recording = true;
	ui->moving = false;
	ui->motion_seen = false;
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
	if (ui->recording) {
		/* Запис і фільтр руху починаються заново: попереднього руху ніби не було. */
		ui->moving = false;
		ui->motion_seen = false;
	}

	return true;
}

void recording_ui_set_moving(struct recording_ui *ui, bool moving)
{
	ui->moving = moving;
	if (moving) {
		ui->motion_seen = true;
	}
}

enum ui_view recording_ui_view(const struct recording_ui *ui)
{
	if (!ui->recording) {
		return UI_VIEW_PAUSED;
	}
	if (ui->moving) {
		return UI_VIEW_MOVING;
	}

	return ui->motion_seen ? UI_VIEW_AFTER : UI_VIEW_BEFORE;
}
