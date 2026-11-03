/**
 * @file device_status.c
 * @brief Реалізація стану пристрою і правил світлодіодів. Опис дивись у device_status.h.
 */
#include "device_status.h"

/* Зелений блимає з періодом 1 с (половину часу горить), синій без фіксу з періодом 0,5 с. */
#define GREEN_BLINK_PERIOD_MS 1000
#define BLUE_BLINK_PERIOD_MS 500

static bool is_stale(int64_t last_ms, int64_t start_ms, int64_t now_ms, int64_t limit_ms);
static bool blink(int64_t now_ms, int64_t period_ms);

void device_status_init(struct device_status *status, bool expect_accel, bool expect_gps,
			int64_t now_ms)
{
	status->expect_accel = expect_accel;
	status->expect_gps = expect_gps;
	atomic_init(&status->start_ms, now_ms);
	atomic_init(&status->last_accel_ms, 0);
	atomic_init(&status->moving, false);
	atomic_init(&status->last_gps_ms, 0);
	atomic_init(&status->gps_fix, false);
	atomic_init(&status->last_failure_ms, 0);
}

struct device_status_snapshot device_status_snapshot(const struct device_status *status)
{
	struct device_status_snapshot copy = {
	    .expect_accel = status->expect_accel,
	    .expect_gps = status->expect_gps,
	    .start_ms = atomic_load(&status->start_ms),
	    .last_accel_ms = atomic_load(&status->last_accel_ms),
	    .moving = atomic_load(&status->moving),
	    .last_gps_ms = atomic_load(&status->last_gps_ms),
	    .gps_fix = atomic_load(&status->gps_fix),
	    .last_failure_ms = atomic_load(&status->last_failure_ms),
	};

	return copy;
}

struct status_leds status_leds_compute(const struct device_status_snapshot *status, int64_t now_ms)
{
	struct status_leds leds = {false, false, false};
	bool accel_stale = status->expect_accel && is_stale(status->last_accel_ms, status->start_ms,
							    now_ms, DEVICE_STATUS_ACCEL_STALE_MS);
	bool gps_stale = status->expect_gps && is_stale(status->last_gps_ms, status->start_ms,
							now_ms, DEVICE_STATUS_GPS_STALE_MS);
	bool recent_failure = status->last_failure_ms != 0 &&
			      now_ms - status->last_failure_ms <= DEVICE_STATUS_FAILURE_HOLD_MS;

	leds.red = accel_stale || gps_stale || recent_failure;

	if (!accel_stale) {
		bool moving = status->expect_accel && status->moving;

		leds.green = moving || blink(now_ms, GREEN_BLINK_PERIOD_MS);
	}

	/* Поки GPS ще жодного разу не відповів, синій не горить: показувати нічого. */
	if (status->expect_gps && !gps_stale && status->last_gps_ms != 0) {
		leds.blue = status->gps_fix || blink(now_ms, BLUE_BLINK_PERIOD_MS);
	}

	return leds;
}

/* Джерело мовчить довше за limit_ms; поки не відповіло жодного разу, рахуємо від старту. */
static bool is_stale(int64_t last_ms, int64_t start_ms, int64_t now_ms, int64_t limit_ms)
{
	int64_t since = last_ms != 0 ? last_ms : start_ms;

	return now_ms - since > limit_ms;
}

/* Перша половина кожного періоду: горить, друга: ні. */
static bool blink(int64_t now_ms, int64_t period_ms)
{
	int64_t phase = now_ms % period_ms;

	if (phase < 0) {
		phase += period_ms;
	}

	return phase < period_ms / 2;
}
