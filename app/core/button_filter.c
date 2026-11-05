/**
 * @file button_filter.c
 * @brief Реалізація фільтра кнопки. Опис дивись у button_filter.h.
 */
#include "button_filter.h"

void button_filter_init(struct button_filter *filter) { filter->down = false; }

bool button_filter_update(struct button_filter *filter, bool active)
{
	bool pressed = active && !filter->down;

	filter->down = active;

	return pressed;
}
