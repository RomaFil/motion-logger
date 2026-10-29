/**
 * @file sensor.h
 * @brief Інтерфейс між «що робить програма» (core/) і «звідки беруться відліки» (drivers/).
 *
 * Ядро програми (буфер, фільтр руху, лог) нічого не знає про залізо. Воно вміє лише дві речі:
 *   - попросити в датчика наступний відлік акселерометра (struct sensor);
 *   - покласти в буфер і записати в лог готовий запис (struct sample).
 * Завдяки цьому «програвач» CSV-файлу з драйвера csv_player.c підставляється замість
 * справжнього датчика, а коли з'явиться читання MPU6050, ядро не доведеться міняти.
 *
 * Правило залежностей: core/ підключає цей файл, drivers/ підключають цей файл, але core/
 * ніколи не підключає файли з drivers/. Підключення драйвера до ядра робить лише main.c.
 */
#ifndef SENSOR_H
#define SENSOR_H

#include <stdbool.h>
#include <stdint.h>

/** @brief Один відлік акселерометра. */
struct accel_sample {
	int64_t time_ms;      /**< мілісекунди за годинником датчика (для запису з файлу: від його
				 початку) */
	double x_g;	      /**< прискорення по X, g */
	double y_g;	      /**< прискорення по Y, g */
	double z_g;	      /**< прискорення по Z, g */
	bool has_temperature; /**< чи драйвер віддав температуру датчика */
	double temperature_c; /**< температура датчика, °C (лише коли has_temperature) */
};

/** @brief Звідки взято запис у буфері. */
enum sample_source {
	SAMPLE_SOURCE_SIM,    /**< імітатор sensor_sim: лише число-лічильник */
	SAMPLE_SOURCE_ACCEL,  /**< основний акселерометр: три осі в g, з нього рахується рух */
	SAMPLE_SOURCE_ACCEL2, /**< додатковий акселерометр: три осі в g, лише для журналу */
	SAMPLE_SOURCE_GPS,    /**< GPS: позиція й швидкість (розібраний рядок RMC) */
	SAMPLE_SOURCE_GYRO, /**< гіроскоп: x_g, y_g, z_g тут у градусах за секунду, плюс температура
			     */
};

/**
 * @brief Запис, який іде через кільцевий буфер від джерела до записувача логу.
 *
 * Поле source каже, які з решти полів мають зміст: для SIM заповнено value, для ACCEL і ACCEL2
 * заповнено x_g, y_g, z_g, для GPS заповнено gps_fix, latitude_deg, longitude_deg, speed_knots,
 * satellites. Решта полів нулі. Мітка часу ставиться там, де відлік з'явився (а не в момент
 * запису в файл), тому затримка в буфері не псує час.
 */
struct sample {
	enum sample_source source;
	int64_t time_ms; /**< момент відліку, мілісекунди */
	int32_t value;	 /**< число імітатора (лише SAMPLE_SOURCE_SIM) */
	double x_g;	 /**< (лише ACCEL і ACCEL2) */
	double y_g;
	double z_g;
	bool gps_fix;	      /**< (лише GPS) чи модуль вважає позицію дійсною (RMC статус A) */
	double latitude_deg;  /**< (лише GPS, коли gps_fix) десяткові градуси, південь мінусом */
	double longitude_deg; /**< (лише GPS, коли gps_fix) десяткові градуси, захід мінусом */
	double speed_knots;   /**< (лише GPS, коли gps_fix) вузли */
	int satellites; /**< (лише GPS) скільки супутників у останньому GGA, або -1, якщо ще не було
			 */
	bool has_utc;	/**< (лише GPS, коли gps_fix) чи є дата й час UTC з рядка RMC */
	int64_t utc_epoch_s;  /**< секунди з 1970-01-01T00:00:00Z, коли has_utc */
	bool has_course;      /**< (лише GPS, коли gps_fix) чи є курс */
	double course_deg;    /**< курс відносно півночі, градуси */
	bool has_altitude;    /**< (лише GPS, коли gps_fix) чи є висота з останнього GGA */
	double altitude_m;    /**< висота над рівнем моря, метри */
	bool has_hdop;	      /**< (лише GPS, коли gps_fix) чи є HDOP з останнього GGA */
	double hdop;	      /**< горизонтальний розкид: чим менше, тим точніше (до 2 добре) */
	bool has_temperature; /**< (лише GYRO) чи є температура датчика */
	double temperature_c; /**< (лише GYRO) температура датчика, °C */
};

/**
 * @brief Джерело відліків акселерометра. Драйвер вбудовує цю структуру першим полем у свою
 *        і заповнює read.
 */
struct sensor {
	/**
	 * Віддає наступний відлік. Може блокуватись, поки відлік не буде готовий.
	 * @return 0, якщо відлік записано в out; -ENODATA, якщо дані скінчились (звичайний
	 *         кінець запису); інакше мінус код помилки
	 */
	int (*read)(struct sensor *self, struct accel_sample *out);
	/** Звільняє все, що відкрив драйвер. Можна викликати лише один раз. */
	void (*close)(struct sensor *self);
};

#endif /* SENSOR_H */
