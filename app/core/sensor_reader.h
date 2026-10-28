/**
 * @file sensor_reader.h
 * @brief Бере відліки акселерометра в «датчика» (struct sensor) і складає їх у кільцевий буфер.
 *
 * Це пара до sample_reader: той читає числа з дескриптора (імітатор), цей питає в об'єкта
 * struct sensor «дай наступний відлік». Що за об'єктом (програвач CSV, пізніше справжній
 * MPU6050), цьому модулю байдуже: так ядро програми відокремлене від заліза.
 *
 * Кожен відлік пакується в struct sample (джерело ACCEL) з міткою часу від датчика.
 */
#ifndef SENSOR_READER_H
#define SENSOR_READER_H

#include <signal.h>
#include <stddef.h>

#include "ring_buffer.h"
#include "sensor.h"

/** @brief Підсумок роботи читача. */
struct sensor_reader_stats {
	size_t received; /**< скільки відліків отримано від датчика */
	size_t dropped;	 /**< скільки з них не влізло в повний буфер і втрачено */
};

/**
 * @brief Читає датчик, поки дані не скінчаться або не з'явиться прохання зупинитись.
 *
 * Функція блокує викликача. Відлік, що не влізло в повний буфер, рахується в stats->dropped
 * і відкидається (як і в sample_reader: читач не затримує джерело, втрату видно в статистиці).
 * Прапорець зупинки перевіряється перед кожним відліком; датчик, що блокується надовго,
 * має сам повертатися (див. -EINTR нижче).
 *
 * @param sensor датчик з заповненим read
 * @param rb     буфер, ініціалізований через ring_buffer_init()
 * @param stop   прапорець зупинки
 * @param stats  сюди пишеться підсумок; поля обнуляються на початку
 * @return 0, якщо дані скінчились (-ENODATA від датчика) або була зупинка; -EINVAL, якщо
 *         будь-який покажчик NULL; інакше мінус код помилки датчика. Код -EINTR від
 *         датчика не вважається помилкою: читач повертається до перевірки прапорця зупинки.
 */
int sensor_reader_run(struct sensor *sensor, struct ring_buffer *rb,
		      const volatile sig_atomic_t *stop, struct sensor_reader_stats *stats);

#endif /* SENSOR_READER_H */
