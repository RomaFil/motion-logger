/**
 * @file device_status.h
 * @brief Короткий стан пристрою для світлодіодів: чи йдуть відліки, чи є рух, чи є GPS-фікс.
 *
 * Стан оновлює потік-записувач логу (він бачить усі записи з буфера), а читає потік світлодіодів.
 * Тому поля атомарні: замок не потрібен, а читач завжди бачить ціле значення. Моменти часу це
 * монотонний годинник у мілісекундах (monotonic_ms()) у момент, коли записувач обробив запис.
 *
 * Що означають світлодіоди, вирішує status_leds_compute(): це чиста функція від знімка стану,
 * тому її легко перевірити тестом без заліза.
 */
#ifndef DEVICE_STATUS_H
#define DEVICE_STATUS_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

/** @brief Скільки мілісекунд без відліків акселерометра вважається «датчик мовчить». */
#define DEVICE_STATUS_ACCEL_STALE_MS 2000
/** @brief Скільки мілісекунд без рядків GPS вважається «GPS мовчить» (NEO-6M шле раз на секунду).
 */
#define DEVICE_STATUS_GPS_STALE_MS 3000
/** @brief Скільки мілісекунд після помилки запису горить червоний. */
#define DEVICE_STATUS_FAILURE_HOLD_MS 5000

/** @brief Спільний стан. Поля змінюй функціями device_status_* або атомарними операціями. */
struct device_status {
	/** чи задано основне джерело акселерометра (ставиться до старту потоків) */
	bool expect_accel;
	/** чи задано GPS (ставиться до старту потоків) */
	bool expect_gps;
	/** коли програма стартувала */
	atomic_llong start_ms;
	/** останній відлік основного акселерометра, 0 = ще не було */
	atomic_llong last_accel_ms;
	/** стан руху після останньої події MOVE_START / MOVE_STOP */
	atomic_bool moving;
	/** останній рядок GPS, 0 = ще не було */
	atomic_llong last_gps_ms;
	/** чи був фікс в останньому рядку GPS */
	atomic_bool gps_fix;
	/** остання помилка запису в лог, 0 = не було */
	atomic_llong last_failure_ms;
};

/** @brief Звичайна (не атомарна) копія стану в один момент: вхід для status_leds_compute(). */
struct device_status_snapshot {
	bool expect_accel;
	bool expect_gps;
	int64_t start_ms;
	int64_t last_accel_ms;
	bool moving;
	int64_t last_gps_ms;
	bool gps_fix;
	int64_t last_failure_ms;
};

/** @brief Стан трьох світлодіодів плати. */
struct status_leds {
	bool red;   /**< помилка: основний датчик чи GPS мовчить, або лог не пишеться */
	bool green; /**< блимає раз на секунду: логер працює; горить: плата рухається */
	bool blue;  /**< горить: GPS має фікс; часто блимає: GPS відповідає, але фіксу ще нема */
};

/** @brief Обнуляє стан і запам'ятовує момент старту. */
void device_status_init(struct device_status *status, bool expect_accel, bool expect_gps,
			int64_t now_ms);

/** @brief Атомарно знімає копію всіх полів. */
struct device_status_snapshot device_status_snapshot(const struct device_status *status);

/**
 * @brief Що мають показувати світлодіоди в момент now_ms.
 *
 * Поки джерело ще жодного разу не відповіло, відлік «мовчання» йде від моменту старту: перші
 * секунди після запуску червоний не горить.
 */
struct status_leds status_leds_compute(const struct device_status_snapshot *status, int64_t now_ms);

#endif /* DEVICE_STATUS_H */
