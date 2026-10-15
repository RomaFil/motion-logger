/**
 * @file ring_buffer.h
 * @brief Кільцевий буфер фіксованого розміру для чисел int32_t.
 *
 * Кільцевий буфер це черга «першим прийшов, першим пішов» (FIFO) у масиві
 * фіксованого розміру. Коли індекс доходить до кінця масиву, він повертається на
 * початок, ніби масив скручений у кільце. Тому буферу не потрібна динамічна
 * пам'ять (malloc), і він працює за однаковий короткий час.
 *
 * Буфер НЕ перезаписує старі дані: якщо він повний, push повертає помилку.
 * Він також НЕ потокобезпечний: цей крок має лише один потік. Замок (mutex)
 * додається в кроці 2.3.
 *
 * Пам'яттю під елементи володіє той, хто викликає: ring_buffer_init() лише
 * запам'ятовує вказівник на масив і не звільняє його.
 */
#ifndef RING_BUFFER_H
#define RING_BUFFER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @brief Стан кільцевого буфера. Поля змінюй лише через функції ring_buffer_*.
 *
 * Ідея: head вказує, КУДИ писати наступний елемент, tail вказує, ЗВІДКИ читати
 * наступний. Лічильник count потрібен, щоб відрізнити порожній буфер від повного:
 * у обох випадках head == tail.
 */
struct ring_buffer {
	int32_t *items;	 /**< масив під елементи (належить викликачеві) */
	size_t capacity; /**< скільки елементів вміщується в масив */
	size_t head;	 /**< індекс для наступного запису */
	size_t tail;	 /**< індекс для наступного читання */
	size_t count;	 /**< скільки елементів лежить у буфері зараз */
};

/**
 * @brief Готує буфер до роботи.
 *
 * @param rb       буфер, який треба ініціалізувати
 * @param storage  масив під елементи; має жити довше за буфер
 * @param capacity розмір масиву в елементах (не в байтах), більше нуля
 * @return 0 при успіху, -EINVAL, якщо rb чи storage рівні NULL або capacity дорівнює нулю
 */
int ring_buffer_init(struct ring_buffer *rb, int32_t *storage, size_t capacity);

/**
 * @brief Кладе одне число в кінець черги.
 *
 * @param rb    ініціалізований буфер
 * @param value число, яке треба покласти
 * @return 0 при успіху, -ENOSPC, якщо буфер повний (нічого не змінюється),
 *         -EINVAL, якщо rb дорівнює NULL
 */
int ring_buffer_push(struct ring_buffer *rb, int32_t value);

/**
 * @brief Забирає найстаріше число з початку черги.
 *
 * @param rb    ініціалізований буфер
 * @param value куди записати забране число
 * @return 0 при успіху, -ENODATA, якщо буфер порожній (*value не змінюється),
 *         -EINVAL, якщо rb чи value дорівнюють NULL
 */
int ring_buffer_pop(struct ring_buffer *rb, int32_t *value);

/**
 * @brief Повертає, скільки чисел зараз у буфері. Параметр rb не може бути NULL.
 */
size_t ring_buffer_count(const struct ring_buffer *rb);

/**
 * @brief Повертає true, якщо в буфері нічого немає. Параметр rb не може бути NULL.
 */
bool ring_buffer_is_empty(const struct ring_buffer *rb);

/**
 * @brief Повертає true, якщо в буфері нема вільного місця. Параметр rb не може бути NULL.
 */
bool ring_buffer_is_full(const struct ring_buffer *rb);

#endif /* RING_BUFFER_H */
