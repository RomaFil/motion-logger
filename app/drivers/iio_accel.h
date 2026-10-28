/**
 * @file iio_accel.h
 * @brief Драйвер акселерометра через IIO sysfs: читає файли in_accel_{x,y,z}_raw ядра Linux.
 *
 * IIO (Industrial I/O) це підсистема ядра, яка віддає показання датчиків файлами в
 * /sys/bus/iio/devices/iio:deviceN/. Для нашої плати так виглядають і MPU6050 (I2C), і
 * ADXL345 (SPI), коли їх підхопили драйвери ядра за device tree. Драйвер реалізує інтерфейс
 * struct sensor (interfaces/sensor.h), тож решта програми не знає, що це не файл CSV.
 *
 * Що читається:
 *   in_accel_x_raw, in_accel_y_raw, in_accel_z_raw   сирі відліки (ціле число)
 *   in_accel_scale  або  in_accel_{x,y,z}_scale      множник у м/с² на одиницю raw (дробове)
 * g = raw * scale / 9,80665. Масштаб читається один раз при відкритті.
 *
 * Частота: read() сам чекає наступного моменту (абсолютні моменти монотонного годинника, затримка
 * кожного кроку не накопичується). Мітка часу відліку це монотонний годинник у мілісекундах.
 *
 * Модуль не потокобезпечний: користується ним один потік.
 */
#ifndef IIO_ACCEL_H
#define IIO_ACCEL_H

#include <stdint.h>

#include "sensor.h"

/** @brief Найбільша довжина шляху до файлу в каталозі пристрою IIO. */
#define IIO_ACCEL_PATH_SIZE 256

/** @brief Стан драйвера. Поля змінюй лише через функції iio_accel_*. */
struct iio_accel {
	struct sensor base;    /**< ПЕРШЕ поле: покажчик на нього це покажчик на драйвер */
	int raw_fd[3];	       /**< відкриті файли in_accel_{x,y,z}_raw */
	double g_per_count[3]; /**< множник raw -> g для кожної осі */
	int64_t period_ms;
	int64_t next_ms; /**< момент наступного відліку, 0 до першого виклику read */
};

/**
 * @brief Відкриває пристрій IIO і зчитує масштаб.
 *
 * Закрити: dev->base.close(&dev->base).
 *
 * @param dev        драйвер, який треба підготувати; після успіху &dev->base це датчик
 * @param device_dir каталог пристрою, наприклад /sys/bus/iio/devices/iio:device0
 * @param rate_hz    частота опитування, 1..1000 Гц (період цілі мілісекунди)
 * @return 0 при успіху; -EINVAL, якщо dev чи device_dir NULL, шлях задовгий або частота
 *         поза межами; -ENOENT і інші мінус errno, якщо файла нема чи його не відкрити;
 *         -EPROTO, якщо масштаб не додатне скінченне число
 */
int iio_accel_open(struct iio_accel *dev, const char *device_dir, unsigned int rate_hz);

#endif /* IIO_ACCEL_H */
