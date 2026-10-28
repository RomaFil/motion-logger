/**
 * @file motion_events.c
 * @brief Реалізація відстеження подій руху. Опис ідеї дивись у motion_events.h.
 */
#include "motion_events.h"

#include <errno.h>

int motion_events_init(struct motion_events *events, size_t confirm_samples)
{
	if (events == NULL || confirm_samples == 0) {
		return -EINVAL;
	}

	events->moving = false;
	events->confirm_samples = confirm_samples;
	events->streak = 0;

	return 0;
}

enum motion_event motion_events_update(struct motion_events *events, bool moving_now)
{
	if (moving_now == events->moving) {
		/* Відповідь збігається зі станом: будь-яка спроба змінити його перервалась. */
		events->streak = 0;
		return MOTION_EVENT_NONE;
	}

	events->streak++;
	if (events->streak < events->confirm_samples) {
		return MOTION_EVENT_NONE;
	}

	events->moving = moving_now;
	events->streak = 0;

	return moving_now ? MOTION_EVENT_START : MOTION_EVENT_STOP;
}
