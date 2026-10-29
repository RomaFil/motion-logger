/**
 * @file test_ui_controller.c
 * @brief Перевірки контролера кнопки й світлодіода на підробному ui_io.
 *
 * Запуск: make test (звичайний) або make test-tsan. Код виходу 0 означає, що все пройшло.
 */
#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>

#include "check.h"
#include "ui_controller.h"

#define MAX_COLORS 16

/* Скільки перевірок не пройшло. main повертає це як код виходу. */
static int failures;

/* Підробний ui_io: сценарій натискань, журнал кольорів і прохання зупинитись після N очікувань. */
struct fake_io {
	struct ui_io base;
	const int *script; /* що віддає wait_press на кожному виклику: 0, 1 або мінус код */
	int script_length;
	int waits;
	enum ui_color colors[MAX_COLORS];
	int color_count;
	int set_color_error; /* мінус код, який віддає set_color, або 0 */
	bool closed;
	volatile sig_atomic_t *stop;
};

static int fake_wait_press(struct ui_io *self, int timeout_ms)
{
	struct fake_io *io = (struct fake_io *)self;
	int answer = 0;

	(void)timeout_ms;
	if (io->waits < io->script_length) {
		answer = io->script[io->waits];
	}
	io->waits++;
	if (io->waits >= io->script_length) {
		*io->stop = 1; /* сценарій закінчився: просимо контролер зупинитись */
	}

	return answer;
}

static int fake_set_color(struct ui_io *self, enum ui_color color)
{
	struct fake_io *io = (struct fake_io *)self;

	if (io->set_color_error != 0) {
		return io->set_color_error;
	}
	if (io->color_count < MAX_COLORS) {
		io->colors[io->color_count] = color;
	}
	io->color_count++;

	return 0;
}

static void fake_close(struct ui_io *self) { ((struct fake_io *)self)->closed = true; }

/* Що знає «решта програми»: чи йде запис і чи є рух. */
struct fake_app {
	bool recording;
	int recording_calls;
	bool moving;
};

static void set_recording(void *context, bool recording)
{
	struct fake_app *app = context;

	app->recording = recording;
	app->recording_calls++;
}

static bool is_moving(void *context) { return ((struct fake_app *)context)->moving; }

static void make_io(struct fake_io *io, const int *script, int length, volatile sig_atomic_t *stop)
{
	*io = (struct fake_io){.base = {.wait_press = fake_wait_press,
					.set_color = fake_set_color,
					.close = fake_close},
			       .script = script,
			       .script_length = length,
			       .stop = stop};
}

/* Перший крок показує початковий колір, наступні без змін нічого не міняють. */
static void test_first_step_shows_color_and_repeats_are_silent(void)
{
	volatile sig_atomic_t stop = 0;
	struct fake_io io;
	struct fake_app app = {.recording = true};
	struct ui_hooks hooks = {
	    .set_recording = set_recording, .is_moving = is_moving, .context = &app};
	struct ui_controller ctl;

	make_io(&io, NULL, 0, &stop);
	ui_controller_init(&ctl, &io.base, &hooks);
	CHECK(ui_controller_step(&ctl, false, 0) == 0);
	CHECK(ui_controller_step(&ctl, false, 100) == 0);
	CHECK(ui_controller_step(&ctl, false, 200) == 0);

	CHECK(io.color_count == 1);
	CHECK(io.colors[0] == UI_COLOR_GREEN);
}

/* Натискання перемикає запис через гачок і міняє колір; рух робить зелений червоним. */
static void test_press_and_motion_change_color_and_notify_app(void)
{
	volatile sig_atomic_t stop = 0;
	struct fake_io io;
	struct fake_app app = {.recording = true};
	struct ui_hooks hooks = {
	    .set_recording = set_recording, .is_moving = is_moving, .context = &app};
	struct ui_controller ctl;

	make_io(&io, NULL, 0, &stop);
	ui_controller_init(&ctl, &io.base, &hooks);
	CHECK(ui_controller_step(&ctl, false, 0) == 0);

	app.moving = true;
	CHECK(ui_controller_step(&ctl, false, 100) == 0);
	CHECK(io.colors[1] == UI_COLOR_RED);

	CHECK(ui_controller_step(&ctl, true, 1000) == 0);
	CHECK(!app.recording);
	CHECK(app.recording_calls == 1);
	CHECK(io.colors[2] == UI_COLOR_BLUE);

	CHECK(ui_controller_step(&ctl, true, 2000) == 0);
	CHECK(app.recording);
	CHECK(io.colors[3] == UI_COLOR_RED); /* рух усе ще є */
	CHECK(io.color_count == 4);
}

/* Дрижання контактів: друге натискання в межах 200 мс не перемикає запис вдруге. */
static void test_bounce_does_not_toggle_twice(void)
{
	volatile sig_atomic_t stop = 0;
	struct fake_io io;
	struct fake_app app = {.recording = true};
	struct ui_hooks hooks = {
	    .set_recording = set_recording, .is_moving = is_moving, .context = &app};
	struct ui_controller ctl;

	make_io(&io, NULL, 0, &stop);
	ui_controller_init(&ctl, &io.base, &hooks);
	CHECK(ui_controller_step(&ctl, true, 1000) == 0);
	CHECK(ui_controller_step(&ctl, true, 1050) == 0);

	CHECK(app.recording_calls == 1);
	CHECK(!app.recording);
}

/* Помилка світлодіода повертається з кроку, а колір не вважається показаним. */
static void test_led_error_is_reported_and_retried(void)
{
	volatile sig_atomic_t stop = 0;
	struct fake_io io;
	struct fake_app app = {.recording = true};
	struct ui_hooks hooks = {
	    .set_recording = set_recording, .is_moving = is_moving, .context = &app};
	struct ui_controller ctl;

	make_io(&io, NULL, 0, &stop);
	io.set_color_error = -EIO;
	ui_controller_init(&ctl, &io.base, &hooks);
	CHECK(ui_controller_step(&ctl, false, 0) == -EIO);

	io.set_color_error = 0;
	CHECK(ui_controller_step(&ctl, false, 100) == 0);
	CHECK(io.color_count == 1);
}

/* Цикл: сценарій «нічого, натиснули, нічого» і зупинка за прапорцем. */
static void test_run_follows_script_and_stops_on_flag(void)
{
	static const int script[] = {0, 1, 0};
	volatile sig_atomic_t stop = 0;
	struct fake_io io;
	struct fake_app app = {.recording = true};
	struct ui_hooks hooks = {
	    .set_recording = set_recording, .is_moving = is_moving, .context = &app};
	struct ui_controller ctl;

	make_io(&io, script, 3, &stop);
	ui_controller_init(&ctl, &io.base, &hooks);
	CHECK(ui_controller_run(&ctl, &stop) == 0);

	CHECK(io.waits == 3);
	CHECK(!app.recording);
	CHECK(io.color_count == 2); /* зелений, потім синій */
	CHECK(io.colors[0] == UI_COLOR_GREEN);
	CHECK(io.colors[1] == UI_COLOR_BLUE);
}

/* Помилка очікування кнопки зупиняє цикл і повертається як є. */
static void test_run_returns_wait_error(void)
{
	static const int script[] = {0, -EIO};
	volatile sig_atomic_t stop = 0;
	struct fake_io io;
	struct fake_app app = {.recording = true};
	struct ui_hooks hooks = {
	    .set_recording = set_recording, .is_moving = is_moving, .context = &app};
	struct ui_controller ctl;

	make_io(&io, script, 3, &stop); /* довжина 3: зупинка не спрацює раніше помилки */
	ui_controller_init(&ctl, &io.base, &hooks);
	CHECK(ui_controller_run(&ctl, &stop) == -EIO);
	CHECK(io.waits == 2);
}

int main(void)
{
	test_first_step_shows_color_and_repeats_are_silent();
	test_press_and_motion_change_color_and_notify_app();
	test_bounce_does_not_toggle_twice();
	test_led_error_is_reported_and_retried();
	test_run_follows_script_and_stops_on_flag();
	test_run_returns_wait_error();

	if (failures != 0) {
		printf("%d check(s) failed\n", failures);
		return 1;
	}
	printf("ui_controller: all checks passed\n");

	return 0;
}
