/**
 * @file test_device_status.c
 * @brief Перевірки правил світлодіодів: що горить при русі, без GPS, коли датчик замовк.
 *
 * Запуск: make test (звичайний) або make test-tsan. Код виходу 0 означає, що все пройшло.
 * Заліза не потрібно: status_leds_compute() чиста функція від знімка стану й часу.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "check.h"
#include "device_status.h"

#define START_MS 100000

/* Скільки перевірок не пройшло. main повертає це як код виходу. */
static int failures;

/* Знімок «усе гаразд»: акселерометр і GPS щойно відповіли, фікс є, руху нема. */
static struct device_status_snapshot healthy(int64_t now_ms)
{
	struct device_status_snapshot s = {.expect_accel = true,
					   .expect_gps = true,
					   .start_ms = START_MS,
					   .last_accel_ms = now_ms - 10,
					   .moving = false,
					   .last_gps_ms = now_ms - 500,
					   .gps_fix = true,
					   .last_failure_ms = 0};

	return s;
}

static void test_healthy_board_blinks_green_and_shows_fix(void)
{
	int64_t on = START_MS + 10000;	/* перша половина секунди */
	int64_t off = START_MS + 10600; /* друга половина */
	struct device_status_snapshot s = healthy(on);
	struct status_leds leds = status_leds_compute(&s, on);

	CHECK(!leds.red && leds.green && leds.blue);
	s = healthy(off);
	leds = status_leds_compute(&s, off);
	CHECK(!leds.red && !leds.green && leds.blue);
}

static void test_moving_board_keeps_green_on(void)
{
	int64_t now = START_MS + 10600;
	struct device_status_snapshot s = healthy(now);
	struct status_leds leds;

	s.moving = true;
	leds = status_leds_compute(&s, now);
	CHECK(leds.green && !leds.red);
}

static void test_gps_without_fix_blinks_blue(void)
{
	int64_t now = START_MS + 10000;
	struct device_status_snapshot s = healthy(now);

	s.gps_fix = false;
	CHECK(status_leds_compute(&s, now).blue);	 /* 0..249 мс періоду 500 */
	CHECK(!status_leds_compute(&s, now + 300).blue); /* 250..499 */
	CHECK(!status_leds_compute(&s, now).red);
}

static void test_silent_accel_turns_red_and_green_off(void)
{
	int64_t now = START_MS + 10000;
	struct device_status_snapshot s = healthy(now);
	struct status_leds leds;

	s.last_accel_ms = now - DEVICE_STATUS_ACCEL_STALE_MS - 1;
	leds = status_leds_compute(&s, now);
	CHECK(leds.red && !leds.green);
}

static void test_silent_gps_turns_red_and_blue_off(void)
{
	int64_t now = START_MS + 10000;
	struct device_status_snapshot s = healthy(now);
	struct status_leds leds;

	s.last_gps_ms = now - DEVICE_STATUS_GPS_STALE_MS - 1;
	leds = status_leds_compute(&s, now);
	CHECK(leds.red && !leds.blue);
}

static void test_recent_failure_holds_red(void)
{
	int64_t now = START_MS + 10000;
	struct device_status_snapshot s = healthy(now);

	s.last_failure_ms = now - DEVICE_STATUS_FAILURE_HOLD_MS;
	CHECK(status_leds_compute(&s, now).red);
	s.last_failure_ms = now - DEVICE_STATUS_FAILURE_HOLD_MS - 1;
	CHECK(!status_leds_compute(&s, now).red);
}

/* Одразу після старту джерела ще не відповідали: червоний не горить, поки не мине поріг. */
static void test_grace_period_after_start(void)
{
	struct device_status_snapshot s = healthy(START_MS);
	struct status_leds leds;

	s.last_accel_ms = 0;
	s.last_gps_ms = 0;
	leds = status_leds_compute(&s, START_MS + 1000);
	CHECK(!leds.red && !leds.blue);
	leds = status_leds_compute(&s, START_MS + DEVICE_STATUS_GPS_STALE_MS + 1);
	CHECK(leds.red);
}

/* Лише GPS, без акселерометра: зелений просто блимає «живий», червоний за акселерометр не горить.
 */
static void test_sources_that_are_not_configured_are_ignored(void)
{
	int64_t now = START_MS + 10000;
	struct device_status_snapshot s = healthy(now);
	struct status_leds leds;

	s.expect_accel = false;
	s.last_accel_ms = 0;
	s.moving = true; /* без акселерометра руху не буває */
	leds = status_leds_compute(&s, now);
	CHECK(!leds.red && leds.green && leds.blue);
	leds = status_leds_compute(&s, now + 600);
	CHECK(!leds.green);

	s.expect_gps = false;
	s.last_gps_ms = 0;
	CHECK(!status_leds_compute(&s, now).blue);
}

static void test_init_and_snapshot_round_trip(void)
{
	struct device_status status;
	struct device_status_snapshot s;

	device_status_init(&status, true, false, 42);
	atomic_store(&status.last_accel_ms, 50);
	atomic_store(&status.moving, true);
	s = device_status_snapshot(&status);
	CHECK(s.expect_accel && !s.expect_gps);
	CHECK(s.start_ms == 42 && s.last_accel_ms == 50 && s.moving);
	CHECK(s.last_gps_ms == 0 && !s.gps_fix && s.last_failure_ms == 0);
}

int main(void)
{
	test_healthy_board_blinks_green_and_shows_fix();
	test_moving_board_keeps_green_on();
	test_gps_without_fix_blinks_blue();
	test_silent_accel_turns_red_and_green_off();
	test_silent_gps_turns_red_and_blue_off();
	test_recent_failure_holds_red();
	test_grace_period_after_start();
	test_sources_that_are_not_configured_are_ignored();
	test_init_and_snapshot_round_trip();

	if (failures != 0) {
		printf("%d check(s) failed\n", failures);
		return 1;
	}
	printf("device_status: all checks passed\n");

	return 0;
}
