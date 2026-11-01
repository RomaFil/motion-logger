/**
 * @file test_stage_indicator.c
 * @brief Перевірки індикатора етапу руху на підробних світлодіодах (без GPIO).
 *
 * Запуск: make test. Код виходу 0 означає, що всі перевірки пройшли.
 */
#include <errno.h>

#include "check.h"
#include "stage_indicator.h"

#define MAX_CALLS 16

/* Підробка: запам'ятовує, які етапи показували, і може відмовитись показувати. */
struct fake_io {
	struct stage_io base;
	enum stage shown[MAX_CALLS];
	int calls;
	int result; /**< що повертає show */
};

/* Скільки перевірок не пройшло. main повертає це як код виходу. */
static int failures;

static int fake_show(struct stage_io *self, enum stage stage)
{
	struct fake_io *fake = (struct fake_io *)self;

	if (fake->calls < MAX_CALLS) {
		fake->shown[fake->calls] = stage;
	}
	fake->calls++;

	return fake->result;
}

static void fake_close(struct stage_io *self) { (void)self; }

static void fake_init(struct fake_io *fake)
{
	fake->base.show = fake_show;
	fake->base.close = fake_close;
	fake->calls = 0;
	fake->result = 0;
}

/* Одразу після init горить «до руху». */
static void test_init_shows_stage_before(void)
{
	struct fake_io fake;
	struct stage_indicator si;

	fake_init(&fake);
	CHECK(stage_indicator_init(&si, &fake.base) == 0);
	CHECK(fake.calls == 1);
	CHECK(fake.shown[0] == STAGE_BEFORE);
}

/* Рух, зупинка, знову рух: етапи міняються за подіями, без подій нічого не відбувається. */
static void test_events_move_through_stages(void)
{
	struct fake_io fake;
	struct stage_indicator si;

	fake_init(&fake);
	CHECK(stage_indicator_init(&si, &fake.base) == 0);

	CHECK(stage_indicator_on_event(&si, MOTION_EVENT_NONE) == 0);
	CHECK(fake.calls == 1);

	CHECK(stage_indicator_on_event(&si, MOTION_EVENT_START) == 0);
	CHECK(fake.calls == 2 && fake.shown[1] == STAGE_MOVING);

	CHECK(stage_indicator_on_event(&si, MOTION_EVENT_STOP) == 0);
	CHECK(fake.calls == 3 && fake.shown[2] == STAGE_AFTER);

	CHECK(stage_indicator_on_event(&si, MOTION_EVENT_START) == 0);
	CHECK(fake.calls == 4 && fake.shown[3] == STAGE_MOVING);
}

/* Подія, що не міняє етап (STOP до першого руху, повторний START), світлодіоди не чіпає. */
static void test_repeated_events_do_not_touch_leds(void)
{
	struct fake_io fake;
	struct stage_indicator si;

	fake_init(&fake);
	CHECK(stage_indicator_init(&si, &fake.base) == 0);

	CHECK(stage_indicator_on_event(&si, MOTION_EVENT_START) == 0);
	CHECK(stage_indicator_on_event(&si, MOTION_EVENT_START) == 0);
	CHECK(fake.calls == 2);

	CHECK(stage_indicator_on_event(&si, MOTION_EVENT_STOP) == 0);
	CHECK(stage_indicator_on_event(&si, MOTION_EVENT_STOP) == 0);
	CHECK(fake.calls == 3);
}

/* Помилка світлодіода повертається викликачеві, а етап не вважається показаним: наступна подія
 * спробує ще раз. */
static void test_led_error_is_reported_and_retried(void)
{
	struct fake_io fake;
	struct stage_indicator si;

	fake_init(&fake);
	CHECK(stage_indicator_init(&si, &fake.base) == 0);

	fake.result = -EIO;
	CHECK(stage_indicator_on_event(&si, MOTION_EVENT_START) == -EIO);
	CHECK(si.current == STAGE_BEFORE);

	fake.result = 0;
	CHECK(stage_indicator_on_event(&si, MOTION_EVENT_START) == 0);
	CHECK(si.current == STAGE_MOVING);
	CHECK(fake.calls == 3);
}

static void test_rejects_null_arguments(void)
{
	struct fake_io fake;
	struct stage_indicator si;

	fake_init(&fake);
	CHECK(stage_indicator_init(NULL, &fake.base) == -EINVAL);
	CHECK(stage_indicator_init(&si, NULL) == -EINVAL);
	CHECK(stage_indicator_on_event(NULL, MOTION_EVENT_START) == -EINVAL);
}

int main(void)
{
	test_init_shows_stage_before();
	test_events_move_through_stages();
	test_repeated_events_do_not_touch_leds();
	test_led_error_is_reported_and_retried();
	test_rejects_null_arguments();

	if (failures != 0) {
		printf("%d check(s) failed\n", failures);
		return 1;
	}
	printf("stage_indicator: all checks passed\n");

	return 0;
}
