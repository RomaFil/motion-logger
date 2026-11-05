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
 *   гіроскоп MPU6050 (SAMPLE_SOURCE_GYRO), не частіше одного рядка на секунду; температура датчика
 *   лише коли драйвер ядра її віддає:
 *     {"t_ms":21000,"src":"gyro","x_dps":0.12,"y_dps":-0.30,"z_dps":0.05,"temp_c":31.4}
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
 *   відкриття файлу (лише з log_writer_options.open_marker): перший рядок після кожного запуску
 *   програми й кожної ротації. Монотонний t_ms після перезавантаження плати починається з нуля;
 *   boot_id (з /proc/sys/kernel/random/boot_id) каже, до якого запуску системи належать рядки
 *   далі, unix_ms це звичайний годинник (без NTP і RTC може бути 1970):
 *     {"t_ms":5120,"src":"log","event":"open","boot_id":"8f0c…","unix_ms":1793734650797}
 *
 * Чому для основного акселерометра не кожен відлік: 100 рядків на секунду це 6 КБ/с, години
 * логу, з якого людині треба вручну витягувати «коли я йшов». Подій і підсумку секунди досить;
 * сирі відліки при потребі лежать у CSV-записі (формат accel_csv), який записувач пише
 * паралельно, якщо задано log_writer_options.raw_path (опція --dump-raw).
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

/** @brief Найбільша довжина шляху до лог-файлу разом із завершальним нулем. */
#define LOG_WRITER_PATH_SIZE 256

/** @brief Найбільше старих файлів при ротації (path.1 … path.99). */
#define LOG_WRITER_MAX_KEEP 99

/** @brief Як часто дані скидаються на картку (fdatasync), мілісекунди. */
#define LOG_WRITER_SYNC_INTERVAL_MS 5000

/** @brief Права нового лог-файлу: лише власник (у лозі координати GPS). */
#define LOG_WRITER_FILE_MODE 0600

/** @brief Підсумок роботи записувача. */
struct log_writer_stats {
	size_t written;	  /**< скільки рядків записано в файл (усіх видів разом) */
	size_t events;	  /**< з них скільки рядків-подій руху */
	size_t rows;	  /**< з них скільки підсумків секунди основного акселерометра */
	size_t aux;	  /**< з них скільки рядків додаткового акселерометра */
	size_t gyro;	  /**< з них скільки рядків гіроскопа */
	size_t gps;	  /**< з них скільки рядків GPS */
	size_t impacts;	  /**< з них скільки рядків FREE_FALL і IMPACT (входять і в events) */
	size_t failed;	  /**< скільки записів не вдалося обробити чи записати */
	size_t paused;	  /**< скільки записів відкинуто, поки запис був на паузі (не помилка) */
	size_t rotations; /**< скільки разів лог-файл ротувався */
};

/** @brief Додаткові можливості записувача; усе необов'язкове. */
struct log_writer_options {
	/**
	 * Файл, куди йдуть СИРІ відліки основного акселерометра у форматі accel_csv
	 * (t_ms,x_g,y_g,z_g, кожен відлік окремим рядком), або NULL. Той самий файл можна віддати
	 * analyze_recording і побачити ті самі події, що й у логу.
	 */
	const char *raw_path;
	/** Ротація: коли лог досяг стількох байтів, він стає path.1 (старі зсуваються); 0 = без. */
	uint64_t max_bytes;
	/** Скільки старих файлів лишати при ротації: 1..LOG_WRITER_MAX_KEEP. */
	unsigned int keep;
	/** Писати рядок "src":"log","event":"open" після відкриття файлу. */
	bool open_marker;
};

/** @brief Стан записувача. Поля змінюй лише через функції log_writer_*. */
struct log_writer {
	struct ring_buffer *source_buffer;
	struct motion_report detector;
	struct impact_detector impact_detector;
	int64_t last_aux_ms;  /**< мітка часу останнього записаного рядка accel2 */
	int64_t last_gyro_ms; /**< мітка часу останнього записаного рядка gyro */
	bool gyro_seen;	      /**< чи писали вже рядок gyro */
	bool aux_seen;	      /**< чи писали вже рядок accel2 */
	int fd;
	int raw_fd; /**< файл сирих відліків, або -1 */
	char path[LOG_WRITER_PATH_SIZE];
	uint64_t max_bytes;
	unsigned int keep;
	bool open_marker;
	uint64_t size;	      /**< поточний розмір лог-файлу */
	bool dirty;	      /**< чи є записане після останнього fdatasync */
	int64_t last_sync_ms; /**< коли востаннє викликали fdatasync */
	pthread_t thread;
	atomic_bool stop_requested;
	atomic_bool paused;	 /**< true: записи з буфера забираються й відкидаються */
	atomic_bool needs_reset; /**< після паузи фільтр руху починає з чистого аркуша */
	atomic_bool moving;	 /**< останній стан руху: true між MOVE_START і MOVE_STOP */
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
 * @brief Складає рядок гіроскопа (град/с) з необов'язковою температурою.
 *
 * @return довжина рядка без завершального нуля; -EINVAL, якщо out чи sample NULL, source не
 *         GYRO або будь-яке число не скінченне; -ENOSPC, якщо рядок не влізає в size
 */
int log_writer_format_gyro(char *out, size_t size, const struct sample *sample);

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
 * @brief Складає рядок відкриття файлу (див. опис формату на початку файлу).
 *
 * @param boot_id ідентифікатор завантаження (36 символів 0-9, a-f, '-'), або NULL: тоді null
 * @param unix_ms звичайний годинник, мілісекунди Unix
 * @return довжина рядка; -EINVAL, якщо out NULL або boot_id недопустимий; -ENOSPC, якщо не
 *         влізає
 */
int log_writer_format_open(char *out, size_t size, int64_t timestamp_ms, const char *boot_id,
			   int64_t unix_ms);

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
 * @brief Те саме, що log_writer_start(), плюс необов'язкові можливості з options.
 *
 * Файл сирих відліків створюється заново (старий вміст зникає), бо два записи в один CSV мали б
 * різний початок відліку часу. Першим рядком іде коментар із назвами стовпців.
 *
 * Ротація (options->max_bytes > 0): коли лог досяг розміру, path.(keep-1) стає path.keep
 * (найстаріший затирається), …, path стає path.1, і запис іде в новий порожній path. Лог
 * скидається на картку (fdatasync) не рідше ніж раз на LOG_WRITER_SYNC_INTERVAL_MS, перед
 * ротацією і при зупинці: при висмикнутому живленні губиться не більше кількох секунд.
 *
 * @param options додаткові можливості, або NULL (тоді це log_writer_start)
 * @return як log_writer_start(); -EINVAL, якщо keep поза 1..LOG_WRITER_MAX_KEEP при
 *         max_bytes > 0; -ENAMETOOLONG, якщо шлях задовгий; ще мінус код помилки відкриття
 *         файлу сирих відліків
 */
int log_writer_start_with(struct log_writer *lw, struct ring_buffer *rb, const char *path,
			  const struct log_writer_options *options);

/**
 * @brief Ставить запис на паузу чи знімає з неї (можна викликати з будь-якого потоку).
 *
 * Поки пауза, записувач забирає записи з буфера, але в лог не пише й у фільтр руху не подає
 * (рахує в stats.paused), тож буфер не переповнюється. Після зняття паузи фільтр руху
 * починає заново: розрив у часі не повинен плутати вікно усереднення.
 */
void log_writer_set_paused(struct log_writer *lw, bool paused);

/** @brief Чи рух зараз зафіксовано: true від MOVE_START до MOVE_STOP (можна з будь-якого потоку).
 */
bool log_writer_is_moving(const struct log_writer *lw);

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
