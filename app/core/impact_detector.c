/**
 * @file impact_detector.c
 * @brief Реалізація виявлення вільного падіння й удару. Опис і пороги дивись у impact_detector.h.
 */
#include "impact_detector.h"

#include <errno.h>
#include <math.h>
#include <string.h>

void impact_detector_init(struct impact_detector *detector)
{
	memset(detector, 0, sizeof(*detector));
}

int impact_detector_add_sample(struct impact_detector *detector, int64_t time_ms, double x_g,
			       double y_g, double z_g, struct impact_event *event)
{
	double magnitude;

	if (detector == NULL || event == NULL || !isfinite(x_g) || !isfinite(y_g) ||
	    !isfinite(z_g) || (detector->has_time && time_ms < detector->last_time_ms)) {
		return -EINVAL;
	}
	detector->has_time = true;
	detector->last_time_ms = time_ms;
	memset(event, 0, sizeof(*event));
	event->kind = IMPACT_EVENT_NONE;

	magnitude = sqrt(x_g * x_g + y_g * y_g + z_g * z_g);

	/* Вільне падіння: невагомість тримається щонайменше мінімальний час. */
	if (magnitude < IMPACT_DETECTOR_FREE_FALL_G) {
		if (!detector->in_free_fall) {
			detector->in_free_fall = true;
			detector->free_fall_reported = false;
			detector->free_fall_start_ms = time_ms;
		}
		if (!detector->free_fall_reported &&
		    time_ms - detector->free_fall_start_ms >= IMPACT_DETECTOR_FREE_FALL_MIN_MS) {
			detector->free_fall_reported = true;
			event->kind = IMPACT_EVENT_FREE_FALL;
			event->magnitude_g = magnitude;
			event->duration_ms = time_ms - detector->free_fall_start_ms;
			return 0;
		}
	} else if (magnitude > IMPACT_DETECTOR_FREE_FALL_EXIT_G) {
		detector->in_free_fall = false; /* гістерезис: між порогами стан не міняється */
	}

	/* Удар: різкий викид, потім пауза, щоб одну дію не рахувати кілька разів. */
	if (magnitude >= IMPACT_DETECTOR_IMPACT_G &&
	    (!detector->impact_seen ||
	     time_ms - detector->last_impact_ms >= IMPACT_DETECTOR_IMPACT_HOLDOFF_MS)) {
		detector->impact_seen = true;
		detector->last_impact_ms = time_ms;
		event->kind = IMPACT_EVENT_IMPACT;
		event->magnitude_g = magnitude;
	}

	return 0;
}
