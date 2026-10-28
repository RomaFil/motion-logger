/**
 * @file log_writer.h
 * @brief Окремий потік: забирає записи з кільцевого буфера й дописує в лог-файл те, що важливо.
 *
 * Один рядок файлу це один запис у форматі JSON (так файл легко читає і людина, і програма).
 * Є два види рядків, залежно від того, звідки взято запис (поле source у struct sample):
 *
 *   імітатор (SAMPLE_SOURCE_SIM), кожне число окремим рядком:
 *     {"t_ms":123456,"src":"sim","value":42}
 *
 *   акселерометр (SAMPLE_SOURCE_ACCEL): рядок лише тоді, коли змінився стан руху.
 *     {"t_ms":20550,"src":"accel","event":"MOVE_START"}
 *     {"t_ms":40550,"src":"accel","event":"MOVE_STOP"}
 *
 * Чому для акселерометра не кожен відлік: 100 рядків на секунду це 6 КБ/с, години логу,
 * з якого людині треба вручну витягувати «коли я йшов». Подій у журналі достатньо; сирі
 * відліки при потребі лежать у CSV-записі (формат accel_csv).
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

#include "motion_events.h"
#include "motion_report.h"
#include "ring_buffer.h"

/** @brief Найбільша довжина імені джерела разом із завершальним нулем. */
#define LOG_WRITER_SOURCE_SIZE 16

/** @brief Найбільша довжина одного рядка логу разом із завершальним нулем. */
#define LOG_WRITER_LINE_SIZE 80

/** @brief Підсумок роботи записувача. */
struct log_writer_stats {
	size_t written; /**< скільки рядків записано в файл (числа імітатора і події руху разом) */
	size_t events;	/**< з них скільки рядків-подій руху */
	size_t failed;	/**< скільки записів не вдалося обробити чи записати */
};

/** @brief Стан записувача. Поля змінюй лише через функції log_writer_*. */
struct log_writer {
	struct ring_buffer *source_buffer;
	struct motion_report detector;
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
