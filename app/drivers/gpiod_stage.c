/**
 * @file gpiod_stage.c
 * @brief Реалізація світлодіодів етапів через libgpiod 1.x. Опис дивись у gpiod_stage.h.
 */
#include "gpiod_stage.h"

#include <errno.h>
#include <gpiod.h>
#include <string.h>

#define CONSUMER "motion_logger"

static int show_stage(struct stage_io *self, enum stage stage);
static void close_stage(struct stage_io *self);
static int request_led(struct gpiod_stage *stage, enum stage index, unsigned int line);
static void release_all(struct gpiod_stage *stage);

int gpiod_stage_open(struct gpiod_stage *stage, const char *chip)
{
	int result;

	if (stage == NULL || chip == NULL) {
		return -EINVAL;
	}
	memset(stage, 0, sizeof(*stage));
	stage->base.show = show_stage;
	stage->base.close = close_stage;

	stage->chip = gpiod_chip_open_lookup(chip);
	if (stage->chip == NULL) {
		return -errno;
	}

	result = request_led(stage, STAGE_BEFORE, GPIOD_STAGE_DEFAULT_BEFORE_LINE);
	if (result < 0) {
		goto fail;
	}
	result = request_led(stage, STAGE_MOVING, GPIOD_STAGE_DEFAULT_MOVING_LINE);
	if (result < 0) {
		goto fail;
	}
	result = request_led(stage, STAGE_AFTER, GPIOD_STAGE_DEFAULT_AFTER_LINE);
	if (result < 0) {
		goto fail;
	}

	return 0;

fail:
	release_all(stage);
	return result;
}

/* Замовляє лінію як вихід, спочатку з нулем: світлодіод вимкнено. */
static int request_led(struct gpiod_stage *stage, enum stage index, unsigned int line)
{
	stage->led[index] = gpiod_chip_get_line(stage->chip, line);
	if (stage->led[index] == NULL) {
		return -errno;
	}

	return gpiod_line_request_output(stage->led[index], CONSUMER, 0) < 0 ? -errno : 0;
}

static int show_stage(struct stage_io *self, enum stage stage)
{
	struct gpiod_stage *driver = (struct gpiod_stage *)self; /* base це перше поле */
	int i;

	for (i = 0; i < GPIOD_STAGE_COUNT; i++) {
		if (gpiod_line_set_value(driver->led[i], i == (int)stage) < 0) {
			return -errno;
		}
	}

	return 0;
}

static void close_stage(struct stage_io *self)
{
	struct gpiod_stage *driver = (struct gpiod_stage *)self;
	int i;

	for (i = 0; i < GPIOD_STAGE_COUNT; i++) {
		if (driver->led[i] != NULL) {
			/* після закриття лінія лишає останній рівень, тож вимикаємо світлодіод */
			(void)gpiod_line_set_value(driver->led[i], 0);
		}
	}
	release_all(driver);
}

static void release_all(struct gpiod_stage *stage)
{
	int i;

	for (i = 0; i < GPIOD_STAGE_COUNT; i++) {
		if (stage->led[i] != NULL) {
			gpiod_line_release(stage->led[i]);
			stage->led[i] = NULL;
		}
	}
	if (stage->chip != NULL) {
		gpiod_chip_close(stage->chip);
		stage->chip = NULL;
	}
}
