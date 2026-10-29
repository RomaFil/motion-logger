/**
 * @file log_writer.h
 * @brief Окремий потік: забирає записи з кільцевого буфера й дописує в лог-файл те, що важливо.
 *
 * Один рядок файлу це один запис у форматі JSON (так файл легко читає і людина, і програма).
 * Види рядків залежать від того, звідки взято запис (поле source у struct sample):
 *
 *   імітатор (SAMPLE_SOURCE_SIM), кожне число окремим рядком:
 *     {"t_ms":123456,"src":"sim","value":42}
 *
 *   основний акселерометр (SAMPLE_SOURCE_ACCEL): подія, коли змінився стан руху, і підсумок
 *   кожної секунди (рухалась плата більшу частину секунди чи ні, середнє відхилення в g):
 *     {"t_ms":20550,"src":"accel","event":"MOVE_START"}
 *     {"t_ms":40550,"src":"accel","event":"MOVE_STOP"}
 *     {"t_ms":21000,"src":"accel","second":20,"moving":true,"avg_dev_g":0.2105}
 *
 *   програмні події за модулем прискорення (|a| = sqrt(x² + y² + z²), у спокої близько 1 g):
 *     {"t_ms":5100,"src":"accel","event":"FREE_FALL","duration_ms":100}  невагомість від 100 мс
 *     {"t_ms":5400,"src":"accel","event":"IMPACT","magnitude_g":4.5000}   удар від 2,5 g
 *   Апаратні переривання датчиків на платі не підключені, тому рахуємо в програмі
 * (impact_detector.h).
 *
 *   додатковий акселерометр (SAMPLE_SOURCE_ACCEL2), не частіше одного рядка на секунду:
 *     {"t_ms":21000,"src":"accel2","x_g":0.0123,"y_g":-0.0042,"z_g":0.9987}
 *
 *   GPS (SAMPLE_SOURCE_GPS), по рядку на кожен рядок RMC від модуля (раз на секунду):
 *     {"t_ms":21000,"src":"gps","fix":true,"lat":50.450100,"lon":30.523400,"speed_knots":0.30,"sats":8}
 *     {"t_ms":3000,"src":"gps","fix":false,"sats":0}
 *   Після основних полів, лише коли відомі й лише з фіксацією, йдуть необов'язкові (порядок
 * сталий): "utc":"2026-10-28T12:00:08Z" (дата й час з супутників), "alt_m":180.0 (висота над рівнем
 *     моря), "course_deg":90.0 (курс), "hdop":1.2 (розкид: до 2 добре, понад 5 погано)
 *   без фіксації координат нема взагалі (нульову широту не можна приймати за реальну точку);
 *   sats дорівнює null, якщо рядка GGA ще не було.
 *
 * Чому для основного акселерометра не кожен відлік: 100 рядків на секунду це 6 КБ/с, години
 * логу, з якого людині треба вручну витягувати «коли я йшов». Подій і підсумку секунди досить;
 * сирі відліки при потребі лежать у CSV-записі (формат accel_csv).
 *
 * Рух визначає фільтр (motion_filter + motion_events), параметри беруться з
 * motion_defaults.h. Це ті самі модулі, що й в analyze_recording, тому лог і звіт збігаються.
 *
 * t_ms це мітка часу запису: для імітатора мілісекунди монотонного годинника (з моменту
 * старту системи; звичайний годинник на Raspberry Pi 5 без батарейки RTC може стрибнути
 * назад), для акселерометра час його власного годинника (для CSV-запису: від початку запису).
 *
 * Кожен рядок іде одним викликом write() у файл, відкритий з O_APPEND, тому рядок не буває
 * «наполовину»: після зупинки програми файл закінчується повним рядком.
 */
#ifndef LOG_WRITER_H
#define LOG_WRITER_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "impact_detector.h"
#include "motion_events.h"
#include "motion_report.h"
#include "ring_buffer.h"

/** @brief Найбільша довжина імені джерела разом із завершальним нулем. */
#define LOG_WRITER_SOURCE_SIZE 16

/** @brief Найбільша довжина одного рядка логу разом із завершальним нулем. */
#define LOG_WRITER_LINE_SIZE 256

/** @brief Підсумок роботи записувача. */
struct log_writer_stats {
	size_t written; /**< скільки рядків записано в файл (усіх видів разом) */
	size_t events;	/**< з них скільки рядків-подій руху */
	size_t rows;	/**< з них скільки підсумків секунди основного акселерометра */
	size_t aux;	/**< з них скільки рядків додаткового акселерометра */
	size_t gps;	/**< з них скільки рядків GPS */
	size_t impacts; /**< з них скільки рядків FREE_FALL і IMPACT (входять і в events) */
	size_t failed;	/**< скільки записів не вдалося обробити чи записати */
};

/** @brief Стан записувача. Поля змінюй лише через функції log_writer_*. */
struct log_writer {
	struct ring_buffer *source_buffer;
	struct motion_report detector;
	struct impact_detector impact_detector;
	int64_t last_aux_ms; /**< мітка часу останнього записаного рядка accel2 */
	bool aux_seen;	     /**< чи писали вже рядок accel2 */
	int fd;
	pthread_t thread;
	atomic_bool stop_requested;
	struct log_writer_stats stats;
};

/**
 * @brief Складає рядок логу для числа імітатора (разом із символом нового рядка).
 *
 * Чиста функція без доступу до файлів і часу, тому її легко перевіряти тестом.
 *
 * @param out          куди писати
 * @param size         розмір масиву out у байтах
 * @param source       ім'я джерела: 1–15 символів a-z, 0-9, _ (інакше JSON довелося б екранувати)
 * @param timestamp_ms мітка часу в мілісекундах
 * @param value        значення відліку
 * @return довжина рядка без завершального нуля; -EINVAL, якщо out чи source NULL або ім'я
 *         джерела недопустиме; -ENOSPC, якщо рядок не влізає в size
 */
int log_writer_format_line(char *out, size_t size, const char *source, int64_t timestamp_ms,
			   int32_t value);

/**
 * @brief Складає рядок логу для події руху (разом із символом нового рядка).
 *
 * @param out          куди писати
 * @param size         розмір масиву out у байтах
 * @param timestamp_ms мітка часу в мілісекундах
 * @param event        MOTION_EVENT_START або MOTION_EVENT_STOP
 * @return довжина рядка без завершального нуля; -EINVAL, якщо out NULL або event не подія
 *         (MOTION_EVENT_NONE); -ENOSPC, якщо рядок не влізає в size
 */
int log_writer_format_event(char *out, size_t size, int64_t timestamp_ms, enum motion_event event);

/**
 * @brief Складає рядок події FREE_FALL чи IMPACT.
 *
 * @return довжина рядка без завершального нуля; -EINVAL, якщо out чи event NULL, kind не подія
 *         або |a| не скінченне число; -ENOSPC, якщо рядок не влізає в size
 */
int log_writer_format_impact(char *out, size_t size, int64_t timestamp_ms,
			     const struct impact_event *event);

/**
 * @brief Складає рядок-підсумок секунди основного акселерометра.
 *
 * @param out          куди писати
 * @param size         розмір масиву out у байтах
 * @param timestamp_ms мітка часу в мілісекундах
 * @param row          підсумок секунди з motion_report
 * @return довжина рядка без завершального нуля; -EINVAL, якщо out чи row NULL або середнє
 *         відхилення не скінченне число; -ENOSPC, якщо рядок не влізає в size
 */
int log_writer_format_row(char *out, size_t size, int64_t timestamp_ms,
			  const struct motion_report_row *row);

/**
 * @brief Складає рядок додаткового акселерометра (осі в g).
 *
 * @return довжина рядка без завершального нуля; -EINVAL, якщо out NULL або будь-яка вісь не
 *         скінченна; -ENOSPC, якщо рядок не влізає в size
 */
int log_writer_format_aux(char *out, size_t size, int64_t timestamp_ms, double x_g, double y_g,
			  double z_g);

/**
 * @brief Складає рядок GPS з запису SAMPLE_SOURCE_GPS.
 *
 * Без фіксації (gps_fix = false) координат у рядку нема. satellites менше нуля дає null.
 *
 * @return довжина рядка без завершального нуля; -EINVAL, якщо out чи sample NULL, source не GPS
 *         або координати чи швидкість не скінченні; -ENOSPC, якщо рядок не влізає в size
 */
int log_writer_format_gps(char *out, size_t size, const struct sample *sample);

/**
 * @brief Відкриває лог-файл і запускає потік, що забирає записи з буфера.
 *
 * Файл створюється, якщо його нема, і дописується в кінець, якщо є.
 *
 * @param lw   записувач, який треба запустити
 * @param rb   буфер, з якого брати записи; має жити довше за записувач
 * @param path шлях до лог-файлу
 * @return 0 при успіху; -EINVAL при NULL; інакше мінус код помилки відкриття файлу
 *         або створення потоку
 */
int log_writer_start(struct log_writer *lw, struct ring_buffer *rb, const char *path);

/**
 * @brief Зупиняє записувач: дописує все, що ще лежить у буфері, закриває файл.
 *
 * Викликай ПІСЛЯ того, як виробники перестали класти записи в буфер: тоді жоден запис
 * не загубиться. Після виклику lw більше не можна використовувати без нового start.
 *
 * @param lw    записувач, запущений через log_writer_start()
 * @param stats сюди пишеться підсумок (можна NULL, якщо підсумок не потрібен)
 * @return 0 при успіху; мінус код помилки закриття файлу чи -EINVAL, якщо lw дорівнює NULL
 */
int log_writer_stop(struct log_writer *lw, struct log_writer_stats *stats);

#endif /* LOG_WRITER_H */
