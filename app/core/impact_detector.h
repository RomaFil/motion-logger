/**
 * @file impact_detector.h
 * @brief Програмне виявлення вільного падіння й удару за модулем прискорення. Чиста логіка.
 *
 * Акселерометри ADXL345 і MPU6050 вміють це апаратно (переривання FREE_FALL, активність, дотик),
 * але на нашій платі виводи INT не підключені, тому ті самі події рахуємо в програмі з потоку
 * відліків. Модуль вектора прискорення |a| = sqrt(x² + y² + z²) у g:
 *   - у спокої |a| ≈ 1 g (гравітація), у вільному падінні |a| ≈ 0;
 *   - удар це короткий викид |a| значно вище 1 g.
 *
 * FREE_FALL: |a| тримається нижче порога IMPACT_DETECTOR_FREE_FALL_G не менше
 * IMPACT_DETECTOR_FREE_FALL_MIN_MS. Подія видається один раз за епізод, у момент, коли мінімальну
 * тривалість досягнуто; наступна можлива, коли |a| повернеться вище порога виходу.
 * IMPACT: |a| досягає IMPACT_DETECTOR_IMPACT_G. Подія видається на першому відліку з перевищенням,
 * і в ній стоїть |a| саме цього відліку (не обов'язково пік удару: при 100 Гц різниця мала). Після
 * удару є пауза IMPACT_DETECTOR_IMPACT_HOLDOFF_MS, щоб одну дію не рахувати кількома ударами.
 *
 * Пороги за довідниками ADXL345 (вільне падіння 0,3-0,6 g, 100-350 мс) і типовими датчиками
 * ударів; на справжньому записі їх підбирають, як і поріг руху.
 */
#ifndef IMPACT_DETECTOR_H
#define IMPACT_DETECTOR_H

#include <stdbool.h>
#include <stdint.h>

/** @brief |a| нижче цього значення вважається невагомістю, g. */
#define IMPACT_DETECTOR_FREE_FALL_G 0.4
/** @brief Вільне падіння закінчилось, коли |a| піднялось вище цього значення, g. */
#define IMPACT_DETECTOR_FREE_FALL_EXIT_G 0.6
/** @brief Скільки мілісекунд невагомості потрібно, щоб це було падіння, а не збій одного відліку.
 */
#define IMPACT_DETECTOR_FREE_FALL_MIN_MS 100
/** @brief |a| від цього значення вважається ударом, g. */
#define IMPACT_DETECTOR_IMPACT_G 2.5
/** @brief Після удару нові удари ігноруються стільки мілісекунд. */
#define IMPACT_DETECTOR_IMPACT_HOLDOFF_MS 500

/** @brief Яка подія сталась на цьому відліку. */
enum impact_event_kind {
	IMPACT_EVENT_NONE,
	IMPACT_EVENT_FREE_FALL, /**< невагомість тривала не менше мінімуму */
	IMPACT_EVENT_IMPACT,	/**< удар */
};

/** @brief Результат обробки одного відліку. */
struct impact_event {
	enum impact_event_kind kind;
	double magnitude_g;  /**< IMPACT: |a| у момент удару; FREE_FALL: |a| на відліку, що закрив
				поріг */
	int64_t duration_ms; /**< FREE_FALL: скільки вже триває невагомість; IMPACT: 0 */
};

/** @brief Стан детектора. Поля змінюй лише через функції impact_detector_*. */
struct impact_detector {
	bool in_free_fall;	 /**< |a| нижче порога падіння з моменту free_fall_start_ms */
	bool free_fall_reported; /**< подію вже видано в цьому епізоді */
	int64_t free_fall_start_ms;
	bool impact_seen; /**< чи вже був удар (для паузи після удару) */
	int64_t last_impact_ms;
	bool has_time;
	int64_t last_time_ms;
};

/** @brief Скидає стан у початковий. */
void impact_detector_init(struct impact_detector *detector);

/**
 * @brief Обробляє один відлік.
 *
 * @param time_ms мітка часу відліку, не менша за попередню
 * @param x_g,y_g,z_g прискорення по осях, g (мають бути скінченні числа)
 * @param event   сюди пишеться подія (kind = IMPACT_EVENT_NONE, коли нічого не сталось)
 * @return 0; -EINVAL, якщо вказівник NULL, час спадає чи число не скінченне
 */
int impact_detector_add_sample(struct impact_detector *detector, int64_t time_ms, double x_g,
			       double y_g, double z_g, struct impact_event *event);

#endif /* IMPACT_DETECTOR_H */
