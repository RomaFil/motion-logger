/**
 * @file ring_buffer.c
 * @brief Реалізація кільцевого буфера. Опис і правила дивись у ring_buffer.h.
 *
 * Правило замка: кожна публічна функція бере замок на початку роботи й віддає
 * його перед виходом. Приватні функції з суфіксом _locked замок самі НЕ беруть:
 * їх викликають лише тоді, коли замок уже взято. Інакше потік чекав би на замок,
 * який тримає сам (mutex не рекурсивний), і завис би назавжди (deadlock).
 */
#include "ring_buffer.h"

#include <errno.h>
#include <pthread.h>

/* Приватні функції описано тут, а визначено внизу: публічні функції йдуть першими. */
static size_t next_index(size_t index, size_t capacity);
static bool is_empty_locked(const struct ring_buffer *rb);
static bool is_full_locked(const struct ring_buffer *rb);

int ring_buffer_init(struct ring_buffer *rb, struct sample *storage, size_t capacity)
{
	int error;

	if (rb == NULL || storage == NULL || capacity == 0) {
		return -EINVAL;
	}

	error = pthread_mutex_init(&rb->lock, NULL);
	if (error != 0) {
		return -error;
	}

	rb->items = storage;
	rb->capacity = capacity;
	rb->head = 0;
	rb->tail = 0;
	rb->count = 0;

	return 0;
}

void ring_buffer_destroy(struct ring_buffer *rb)
{
	if (rb == NULL) {
		return;
	}

	pthread_mutex_destroy(&rb->lock);
}

int ring_buffer_push(struct ring_buffer *rb, const struct sample *item)
{
	int result = 0;

	if (rb == NULL || item == NULL) {
		return -EINVAL;
	}

	pthread_mutex_lock(&rb->lock);
	if (is_full_locked(rb)) {
		/* Старі дані не затираємо: помилку бачить той, хто викликав. */
		result = -ENOSPC;
	} else {
		rb->items[rb->head] = *item;
		rb->head = next_index(rb->head, rb->capacity);
		rb->count++;
	}
	pthread_mutex_unlock(&rb->lock);

	return result;
}

int ring_buffer_pop(struct ring_buffer *rb, struct sample *item)
{
	int result = 0;

	if (rb == NULL || item == NULL) {
		return -EINVAL;
	}

	pthread_mutex_lock(&rb->lock);
	if (is_empty_locked(rb)) {
		result = -ENODATA;
	} else {
		*item = rb->items[rb->tail];
		rb->tail = next_index(rb->tail, rb->capacity);
		rb->count--;
	}
	pthread_mutex_unlock(&rb->lock);

	return result;
}

size_t ring_buffer_count(struct ring_buffer *rb)
{
	size_t count;

	pthread_mutex_lock(&rb->lock);
	count = rb->count;
	pthread_mutex_unlock(&rb->lock);

	return count;
}

bool ring_buffer_is_empty(struct ring_buffer *rb)
{
	bool empty;

	pthread_mutex_lock(&rb->lock);
	empty = is_empty_locked(rb);
	pthread_mutex_unlock(&rb->lock);

	return empty;
}

bool ring_buffer_is_full(struct ring_buffer *rb)
{
	bool full;

	pthread_mutex_lock(&rb->lock);
	full = is_full_locked(rb);
	pthread_mutex_unlock(&rb->lock);

	return full;
}

/* Наступний індекс у кільці: після останнього елемента масиву повертаємось до нульового. */
static size_t next_index(size_t index, size_t capacity)
{
	index++;
	if (index == capacity) {
		index = 0;
	}

	return index;
}

/* Замок має бути вже взятий викликачем. */
static bool is_empty_locked(const struct ring_buffer *rb) { return rb->count == 0; }

/* Замок має бути вже взятий викликачем. */
static bool is_full_locked(const struct ring_buffer *rb) { return rb->count == rb->capacity; }
