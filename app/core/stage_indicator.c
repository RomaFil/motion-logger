/**
 * @file stage_indicator.c
 * @brief Реалізація індикатора етапу руху. Опис дивись у stage_indicator.h.
 */
#include "stage_indicator.h"

#include <errno.h>
#include <stddef.h>

int stage_indicator_init(struct stage_indicator *si, struct stage_io *io)
{
	if (si == NULL || io == NULL) {
		return -EINVAL;
	}

	si->io = io;
	si->current = STAGE_BEFORE;

	return io->show(io, STAGE_BEFORE);
}

int stage_indicator_on_event(struct stage_indicator *si, enum motion_event event)
{
	enum stage next;
	int result;

	if (si == NULL) {
		return -EINVAL;
	}
	if (event == MOTION_EVENT_START) {
		next = STAGE_MOVING;
	} else if (event == MOTION_EVENT_STOP) {
		next = STAGE_AFTER;
	} else {
		return 0;
	}
	if (next == si->current) {
		return 0;
	}

	result = si->io->show(si->io, next);
	if (result == 0) {
		si->current = next;
	}

	return result;
}
