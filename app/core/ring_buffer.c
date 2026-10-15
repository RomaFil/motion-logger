/**
 * @file ring_buffer.c
 * @brief Реалізація кільцевого буфера. Опис і правила дивись у ring_buffer.h.
 */
#include "ring_buffer.h"

#include <errno.h>

/* Приватна функція описана тут, а визначена внизу: публічні функції йдуть першими. */
static size_t next_index(size_t index, size_t capacity);

int ring_buffer_init(struct ring_buffer *rb, int32_t *storage, size_t capacity)
{
	if (rb == NULL || storage == NULL || capacity == 0) {
		return -EINVAL;
	}

	rb->items = storage;
	rb->capacity = capacity;
	rb->head = 0;
	rb->tail = 0;
	rb->count = 0;

	return 0;
}

int ring_buffer_push(struct ring_buffer *rb, int32_t value)
{
	if (rb == NULL) {
		return -EINVAL;
	}
	if (ring_buffer_is_full(rb)) {
		/* Старі дані не затираємо: помилку бачить той, хто викликав. */
		return -ENOSPC;
	}

	rb->items[rb->head] = value;
	rb->head = next_index(rb->head, rb->capacity);
	rb->count++;

	return 0;
}

int ring_buffer_pop(struct ring_buffer *rb, int32_t *value)
{
	if (rb == NULL || value == NULL) {
		return -EINVAL;
	}
	if (ring_buffer_is_empty(rb)) {
		return -ENODATA;
	}

	*value = rb->items[rb->tail];
	rb->tail = next_index(rb->tail, rb->capacity);
	rb->count--;

	return 0;
}

size_t ring_buffer_count(const struct ring_buffer *rb) { return rb->count; }

bool ring_buffer_is_empty(const struct ring_buffer *rb) { return rb->count == 0; }

bool ring_buffer_is_full(const struct ring_buffer *rb) { return rb->count == rb->capacity; }

/* Наступний індекс у кільці: після останнього елемента масиву повертаємось до нульового. */
static size_t next_index(size_t index, size_t capacity)
{
	index++;
	if (index == capacity) {
		index = 0;
	}

	return index;
}
