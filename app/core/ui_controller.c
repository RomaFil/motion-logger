/**
 * @file ui_controller.c
 * @brief Реалізація циклу кнопки й світлодіода. Опис і правила дивись у ui_controller.h.
 */
#include "ui_controller.h"

#include "io_util.h"

void ui_controller_init(struct ui_controller *ctl, struct ui_io *io, const struct ui_hooks *hooks)
{
	recording_ui_init(&ctl->ui);
	ctl->io = io;
	ctl->hooks = *hooks;
	ctl->shown = UI_COLOR_GREEN;
	ctl->shown_valid = false;
}

int ui_controller_step(struct ui_controller *ctl, bool pressed, int64_t now_ms)
{
	enum ui_color color;
	int result;

	if (pressed && recording_ui_press(&ctl->ui, now_ms)) {
		ctl->hooks.set_recording(ctl->hooks.context, ctl->ui.recording);
	}
	recording_ui_set_moving(&ctl->ui, ctl->hooks.is_moving(ctl->hooks.context));

	color = recording_ui_color(&ctl->ui);
	if (ctl->shown_valid && color == ctl->shown) {
		return 0;
	}

	result = ctl->io->set_color(ctl->io, color);
	if (result < 0) {
		return result;
	}
	ctl->shown = color;
	ctl->shown_valid = true;

	return 0;
}

int ui_controller_run(struct ui_controller *ctl, const volatile sig_atomic_t *stop)
{
	int result = ui_controller_step(ctl, false, monotonic_ms()); /* показати початковий колір */

	while (result == 0 && !*stop) {
		int pressed = ctl->io->wait_press(ctl->io, UI_CONTROLLER_WAIT_MS);

		if (pressed < 0) {
			result = pressed;
			break;
		}
		result = ui_controller_step(ctl, pressed > 0, monotonic_ms());
	}

	return result;
}
