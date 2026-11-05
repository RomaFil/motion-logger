/**
 * @file gpiod_ui.c
 * @brief Реалізація кнопки й світлодіодів через libgpiod 1.x. Опис дивись у gpiod_ui.h.
 */
#include "gpiod_ui.h"

#include <errno.h>
#include <gpiod.h>
#include <string.h>
#include <time.h>

#define CONSUMER "motion_logger"
#define MS_PER_SECOND 1000
#define NS_PER_MS 1000000L
#define NS_PER_SECOND 1000000000L

static int wait_press(struct ui_io *self, int timeout_ms);
static int show(struct ui_io *self, enum ui_view view);
static void close_ui(struct ui_io *self);
static int request_button(struct gpiod_ui *ui, unsigned int line, bool active_high);
static int request_led(struct gpiod_ui *ui, int index, unsigned int line);
static void release_all(struct gpiod_ui *ui);
static int settle_and_read_level(struct gpiod_ui *ui);
static bool time_left(const struct timespec *deadline, struct timespec *left);

struct gpiod_ui_config gpiod_ui_default_config(const char *chip)
{
	struct gpiod_ui_config config = {.chip = chip,
					 .use_button = true,
					 .button_line = GPIOD_UI_DEFAULT_BUTTON_LINE,
					 .before_line = GPIOD_UI_DEFAULT_BEFORE_LINE,
					 .moving_line = GPIOD_UI_DEFAULT_MOVING_LINE,
					 .after_line = GPIOD_UI_DEFAULT_AFTER_LINE,
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
	ui->button_active_high = config->button_active_high;
	button_filter_init(&ui->button_filter);
	ui->base.wait_press = wait_press;
	ui->base.show = show;
	ui->base.close = close_ui;

	ui->chip = gpiod_chip_open_lookup(config->chip);
	if (ui->chip == NULL) {
		return -errno;
	}

	if (config->use_button) {
		result = request_button(ui, config->button_line, config->button_active_high);
		if (result < 0) {
			goto fail;
		}
	}
	result = request_led(ui, UI_VIEW_BEFORE, config->before_line);
	if (result < 0) {
		goto fail;
	}
	result = request_led(ui, UI_VIEW_MOVING, config->moving_line);
	if (result < 0) {
		goto fail;
	}
	result = request_led(ui, UI_VIEW_AFTER, config->after_line);
	if (result < 0) {
		goto fail;
	}

	return 0;

fail:
	release_all(ui);
	return result;
}

/*
 * Підтяжка тримає лінію у стані «відпущено»: до землі для кнопки, що дає 1, і до живлення для
 * кнопки, що замикає на землю. Події обох фронтів: за ними драйвер бачить і натискання, і
 * відпускання, а що саме вважати натисканням, вирішує рівень лінії (див. wait_press).
 */
static int request_button(struct gpiod_ui *ui, unsigned int line, bool active_high)
{
	int flags = active_high ? GPIOD_LINE_REQUEST_FLAG_BIAS_PULL_DOWN
				: GPIOD_LINE_REQUEST_FLAG_BIAS_PULL_UP;
	int result;

	ui->button = gpiod_chip_get_line(ui->chip, line);
	if (ui->button == NULL) {
		return -errno;
	}
	result = gpiod_line_request_both_edges_events_flags(ui->button, CONSUMER, flags);

	return result < 0 ? -errno : 0;
}

/* Замовляє лінію як вихід, спочатку з нулем: світлодіод вимкнено. */
static int request_led(struct gpiod_ui *ui, int index, unsigned int line)
{
	ui->led[index] = gpiod_chip_get_line(ui->chip, line);
	if (ui->led[index] == NULL) {
		return -errno;
	}

	return gpiod_line_request_output(ui->led[index], CONSUMER, 0) < 0 ? -errno : 0;
}

/*
 * Чекає натискання до timeout_ms. Кожна подія на лінії (будь-який фронт, зокрема дрижання) лише
 * будить цикл: драйвер чекає, поки контакт заспокоїться, зчитує справжній рівень і питає фільтр, чи
 * це перехід «відпущено → натиснуто». Так відпускання кнопки, навіть із дрижанням, нових натискань
 * не дає.
 */
static int wait_press(struct ui_io *self, int timeout_ms)
{
	struct gpiod_ui *ui = (struct gpiod_ui *)self; /* base це перше поле */
	struct timespec timeout = {.tv_sec = timeout_ms / MS_PER_SECOND,
				   .tv_nsec = (timeout_ms % MS_PER_SECOND) * NS_PER_MS};
	struct timespec deadline;

	if (ui->button == NULL) {
		nanosleep(&timeout,
			  NULL); /* без кнопки просто чекаємо: контролер оновлює світлодіоди */
		return 0;
	}

	(void)clock_gettime(CLOCK_MONOTONIC, &deadline);
	deadline.tv_sec += timeout.tv_sec;
	deadline.tv_nsec += timeout.tv_nsec;
	if (deadline.tv_nsec >= NS_PER_SECOND) {
		deadline.tv_sec++;
		deadline.tv_nsec -= NS_PER_SECOND;
	}

	do {
		int ready = gpiod_line_event_wait(ui->button, &timeout);
		int level;

		if (ready < 0) {
			return errno == EINTR ? 0 : -errno;
		}
		if (ready == 0) {
			return 0;
		}
		level = settle_and_read_level(ui);
		if (level < 0) {
			return level;
		}
		if (button_filter_update(&ui->button_filter,
					 (level != 0) == ui->button_active_high)) {
			return 1;
		}
	} while (time_left(&deadline, &timeout));

	return 0;
}

/* Читає подію, чекає GPIOD_UI_BUTTON_SETTLE_MS, знімає накопичені події дрижання й повертає рівень
 * лінії (0 чи 1). */
static int settle_and_read_level(struct gpiod_ui *ui)
{
	struct gpiod_line_event event;
	struct timespec settle = {.tv_sec = 0, .tv_nsec = GPIOD_UI_BUTTON_SETTLE_MS * NS_PER_MS};
	struct timespec none = {.tv_sec = 0, .tv_nsec = 0};
	int level;

	if (gpiod_line_event_read(ui->button, &event) < 0) {
		return -errno;
	}
	nanosleep(&settle, NULL);
	while (gpiod_line_event_wait(ui->button, &none) > 0) {
		if (gpiod_line_event_read(ui->button, &event) < 0) {
			return -errno;
		}
	}
	level = gpiod_line_get_value(ui->button);

	return level < 0 ? -errno : level;
}

/* Скільки лишилось до deadline; false, якщо час вийшов. */
static bool time_left(const struct timespec *deadline, struct timespec *left)
{
	struct timespec now;

	(void)clock_gettime(CLOCK_MONOTONIC, &now);
	left->tv_sec = deadline->tv_sec - now.tv_sec;
	left->tv_nsec = deadline->tv_nsec - now.tv_nsec;
	if (left->tv_nsec < 0) {
		left->tv_sec--;
		left->tv_nsec += NS_PER_SECOND;
	}

	return left->tv_sec >= 0 && (left->tv_sec > 0 || left->tv_nsec > 0);
}

/* Один етап: горить лише його світлодіод. Пауза: горять усі три. */
static int show(struct ui_io *self, enum ui_view view)
{
	struct gpiod_ui *ui = (struct gpiod_ui *)self;
	int i;

	for (i = 0; i < GPIOD_UI_LED_COUNT; i++) {
		int on = view == UI_VIEW_PAUSED || i == (int)view;

		if (gpiod_line_set_value(ui->led[i], on) < 0) {
			return -errno;
		}
	}

	return 0;
}

static void close_ui(struct ui_io *self)
{
	struct gpiod_ui *ui = (struct gpiod_ui *)self;
	int i;

	for (i = 0; i < GPIOD_UI_LED_COUNT; i++) {
		if (ui->led[i] != NULL) {
			/* після закриття лінія лишає останній рівень, тож вимикаємо світлодіод */
			(void)gpiod_line_set_value(ui->led[i], 0);
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
	for (i = 0; i < GPIOD_UI_LED_COUNT; i++) {
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
