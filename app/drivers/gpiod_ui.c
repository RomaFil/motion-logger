/**
 * @file gpiod_ui.c
 * @brief Реалізація кнопки й світлодіода через libgpiod 1.x. Опис дивись у gpiod_ui.h.
 */
#include "gpiod_ui.h"

#include <errno.h>
#include <gpiod.h>
#include <string.h>
#include <time.h>

#define CONSUMER "motion_logger"
#define LED_COUNT 3
#define MS_PER_SECOND 1000
#define NS_PER_MS 1000000L

enum led_index { LED_RED, LED_GREEN, LED_BLUE };

static int wait_press(struct ui_io *self, int timeout_ms);
static int set_color(struct ui_io *self, enum ui_color color);
static void close_ui(struct ui_io *self);
static int request_button(struct gpiod_ui *ui, unsigned int line, bool active_high);
static int request_led(struct gpiod_ui *ui, enum led_index index, unsigned int line);
static void release_all(struct gpiod_ui *ui);

struct gpiod_ui_config gpiod_ui_default_config(const char *chip)
{
	struct gpiod_ui_config config = {.chip = chip,
					 .button_line = GPIOD_UI_DEFAULT_BUTTON_LINE,
					 .red_line = GPIOD_UI_DEFAULT_RED_LINE,
					 .green_line = GPIOD_UI_DEFAULT_GREEN_LINE,
					 .blue_line = GPIOD_UI_DEFAULT_BLUE_LINE,
					 .button_active_high = true};

	return config;
}

int gpiod_ui_open(struct gpiod_ui *ui, const struct gpiod_ui_config *config)
{
	int result;

	if (ui == NULL || config == NULL || config->chip == NULL) {
		return -EINVAL;
	}
	memset(ui, 0, sizeof(*ui));
	ui->base.wait_press = wait_press;
	ui->base.set_color = set_color;
	ui->base.close = close_ui;

	ui->chip = gpiod_chip_open_lookup(config->chip);
	if (ui->chip == NULL) {
		return -errno;
	}

	result = request_button(ui, config->button_line, config->button_active_high);
	if (result < 0) {
		goto fail;
	}
	result = request_led(ui, LED_RED, config->red_line);
	if (result < 0) {
		goto fail;
	}
	result = request_led(ui, LED_GREEN, config->green_line);
	if (result < 0) {
		goto fail;
	}
	result = request_led(ui, LED_BLUE, config->blue_line);
	if (result < 0) {
		goto fail;
	}

	return 0;

fail:
	release_all(ui);
	return result;
}

/* Натисканням вважається зростання (активний високий рівень) чи спад напруги на лінії кнопки. */
static int request_button(struct gpiod_ui *ui, unsigned int line, bool active_high)
{
	int flags = active_high ? GPIOD_LINE_REQUEST_FLAG_BIAS_PULL_DOWN
				: GPIOD_LINE_REQUEST_FLAG_BIAS_PULL_UP;
	int result;

	ui->button = gpiod_chip_get_line(ui->chip, line);
	if (ui->button == NULL) {
		return -errno;
	}
	if (active_high) {
		result = gpiod_line_request_rising_edge_events_flags(ui->button, CONSUMER, flags);
	} else {
		result = gpiod_line_request_falling_edge_events_flags(ui->button, CONSUMER, flags);
	}

	return result < 0 ? -errno : 0;
}

static int request_led(struct gpiod_ui *ui, enum led_index index, unsigned int line)
{
	ui->led[index] = gpiod_chip_get_line(ui->chip, line);
	if (ui->led[index] == NULL) {
		return -errno;
	}

	return gpiod_line_request_output(ui->led[index], CONSUMER, 0) < 0 ? -errno : 0;
}

static int wait_press(struct ui_io *self, int timeout_ms)
{
	struct gpiod_ui *ui = (struct gpiod_ui *)self; /* base це перше поле */
	struct timespec timeout = {.tv_sec = timeout_ms / MS_PER_SECOND,
				   .tv_nsec = (timeout_ms % MS_PER_SECOND) * NS_PER_MS};
	struct gpiod_line_event event;
	int ready = gpiod_line_event_wait(ui->button, &timeout);

	if (ready < 0) {
		return errno == EINTR ? 0 : -errno;
	}
	if (ready == 0) {
		return 0;
	}
	if (gpiod_line_event_read(ui->button, &event) < 0) {
		return -errno;
	}

	return 1;
}

static int set_color(struct ui_io *self, enum ui_color color)
{
	struct gpiod_ui *ui = (struct gpiod_ui *)self;
	int levels[LED_COUNT] = {color == UI_COLOR_RED, color == UI_COLOR_GREEN,
				 color == UI_COLOR_BLUE};
	int i;

	for (i = 0; i < LED_COUNT; i++) {
		if (gpiod_line_set_value(ui->led[i], levels[i]) < 0) {
			return -errno;
		}
	}

	return 0;
}

static void close_ui(struct ui_io *self)
{
	struct gpiod_ui *ui = (struct gpiod_ui *)self;
	int i;

	for (i = 0; i < LED_COUNT; i++) {
		if (ui->led[i] != NULL) {
			(void)gpiod_line_set_value(ui->led[i],
						   0); /* світлодіод не лишаємо ввімкненим */
		}
	}
	release_all(ui);
}

static void release_all(struct gpiod_ui *ui)
{
	int i;

	if (ui->button != NULL) {
		gpiod_line_release(ui->button);
		ui->button = NULL;
	}
	for (i = 0; i < LED_COUNT; i++) {
		if (ui->led[i] != NULL) {
			gpiod_line_release(ui->led[i]);
			ui->led[i] = NULL;
		}
	}
	if (ui->chip != NULL) {
		gpiod_chip_close(ui->chip);
		ui->chip = NULL;
	}
}
