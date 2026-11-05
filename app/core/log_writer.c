/**
 * @file log_writer.c
 * @brief Реалізація записувача логу. Опис формату й правил дивись у log_writer.h.
 */
#include "log_writer.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "accel_csv.h"
#include "io_util.h"
#include "motion_defaults.h"
#include "sensor.h"

/* Як довго потік спить, коли в буфері порожньо. 10 мс = не більше ~100 прокидань на секунду. */
#define IDLE_SLEEP_MS 10

#define NSEC_PER_MSEC 1000000L

/* Ім'я джерела в рядках логу. Лише a-z, 0-9, _: воно йде в JSON без екранування. */
#define SIM_SOURCE_NAME "sim"
#define ACCEL_SOURCE_NAME "accel"
#define ACCEL2_SOURCE_NAME "accel2"
#define GPS_SOURCE_NAME "gps"

/* Додатковий акселерометр пишемо не частіше за цей інтервал: 100 відліків на секунду в лог не
 * потрібні. */
#define AUX_PERIOD_MS 1000

#define LOG_FILE_MODE LOG_WRITER_FILE_MODE
#define LOG_SOURCE_NAME "log"

/* /proc/sys/kernel/random/boot_id: 36 символів UUID і '\n'. */
#define BOOT_ID_PATH "/proc/sys/kernel/random/boot_id"
#define BOOT_ID_LENGTH 36
#define MSEC_PER_SEC 1000

/* Перший рядок файлу сирих відліків: коментар, accel_csv його пропускає. */
#define RAW_HEADER "# t_ms,x_g,y_g,z_g\n"

/* Приватні функції описано тут, а визначено внизу: публічні функції йдуть першими. */
static bool is_valid_source(const char *source);
static void *run_writer_thread(void *argument);
static void handle_sample(struct log_writer *lw, const struct sample *item);
static void write_sim_sample(struct log_writer *lw, const struct sample *item);
static void write_accel_sample(struct log_writer *lw, const struct sample *item);
static void write_aux_sample(struct log_writer *lw, const struct sample *item);
static void write_gps_sample(struct log_writer *lw, const struct sample *item);
static void write_line(struct log_writer *lw, const char *line, int length);
static void write_open_marker(struct log_writer *lw);
static int open_log(struct log_writer *lw);
static void rotate(struct log_writer *lw);
static void sync_if_due(struct log_writer *lw, bool force);
static bool is_valid_boot_id(const char *boot_id);
static void read_boot_id(char *out, size_t size);
static int64_t realtime_ms(void);
static int init_detector(struct log_writer *lw);
static int open_raw_file(const char *path);
static void write_raw_sample(struct log_writer *lw, const struct sample *item);
static void sleep_idle(void);

int log_writer_format_line(char *out, size_t size, const char *source, int64_t timestamp_ms,
			   int32_t value)
{
	int length;

	if (out == NULL || !is_valid_source(source)) {
		return -EINVAL;
	}

	length =
	    snprintf(out, size, "{\"t_ms\":%" PRId64 ",\"src\":\"%s\",\"value\":%" PRId32 "}\n",
		     timestamp_ms, source, value);
	if (length < 0 || (size_t)length >= size) {
		return -ENOSPC;
	}

	return length;
}

int log_writer_format_event(char *out, size_t size, int64_t timestamp_ms, enum motion_event event)
{
	const char *name;
	int length;

	if (out == NULL) {
		return -EINVAL;
	}
	if (event == MOTION_EVENT_START) {
		name = "MOVE_START";
	} else if (event == MOTION_EVENT_STOP) {
		name = "MOVE_STOP";
	} else {
		return -EINVAL;
	}

	length = snprintf(out, size, "{\"t_ms\":%" PRId64 ",\"src\":\"%s\",\"event\":\"%s\"}\n",
			  timestamp_ms, ACCEL_SOURCE_NAME, name);
	if (length < 0 || (size_t)length >= size) {
		return -ENOSPC;
	}

	return length;
}

int log_writer_format_row(char *out, size_t size, int64_t timestamp_ms,
			  const struct motion_report_row *row)
{
	int length;

	if (out == NULL || row == NULL || !isfinite(row->average_deviation_g)) {
		return -EINVAL;
	}

	length = snprintf(out, size,
			  "{\"t_ms\":%" PRId64 ",\"src\":\"%s\",\"second\":%" PRId64
			  ",\"moving\":%s,\"avg_dev_g\":%.4f}\n",
			  timestamp_ms, ACCEL_SOURCE_NAME, row->second,
			  row->moving ? "true" : "false", row->average_deviation_g);
	if (length < 0 || (size_t)length >= size) {
		return -ENOSPC;
	}

	return length;
}

int log_writer_format_aux(char *out, size_t size, int64_t timestamp_ms, double x_g, double y_g,
			  double z_g)
{
	int length;

	if (out == NULL || !isfinite(x_g) || !isfinite(y_g) || !isfinite(z_g)) {
		return -EINVAL;
	}

	length = snprintf(out, size,
			  "{\"t_ms\":%" PRId64
			  ",\"src\":\"%s\",\"x_g\":%.4f,\"y_g\":%.4f,\"z_g\":%.4f}\n",
			  timestamp_ms, ACCEL2_SOURCE_NAME, x_g, y_g, z_g);
	if (length < 0 || (size_t)length >= size) {
		return -ENOSPC;
	}

	return length;
}

int log_writer_format_gps(char *out, size_t size, const struct sample *sample)
{
	char sats[16];
	int length;

	if (out == NULL || sample == NULL || sample->source != SAMPLE_SOURCE_GPS) {
		return -EINVAL;
	}
	if (sample->satellites < 0) {
		snprintf(sats, sizeof(sats), "null");
	} else {
		snprintf(sats, sizeof(sats), "%d", sample->satellites);
	}

	if (!sample->gps_fix) {
		length = snprintf(
		    out, size, "{\"t_ms\":%" PRId64 ",\"src\":\"%s\",\"fix\":false,\"sats\":%s}\n",
		    sample->time_ms, GPS_SOURCE_NAME, sats);
	} else if (!isfinite(sample->latitude_deg) || !isfinite(sample->longitude_deg) ||
		   !isfinite(sample->speed_knots)) {
		return -EINVAL;
	} else {
		length = snprintf(out, size,
				  "{\"t_ms\":%" PRId64
				  ",\"src\":\"%s\",\"fix\":true,\"lat\":%.6f,\"lon\":%.6f,"
				  "\"speed_knots\":%.2f,\"sats\":%s}\n",
				  sample->time_ms, GPS_SOURCE_NAME, sample->latitude_deg,
				  sample->longitude_deg, sample->speed_knots, sats);
	}
	if (length < 0 || (size_t)length >= size) {
		return -ENOSPC;
	}

	return length;
}

int log_writer_format_open(char *out, size_t size, int64_t timestamp_ms, const char *boot_id,
			   int64_t unix_ms)
{
	char quoted[BOOT_ID_LENGTH + 3];
	int length;

	if (out == NULL || (boot_id != NULL && !is_valid_boot_id(boot_id))) {
		return -EINVAL;
	}
	if (boot_id != NULL) {
		snprintf(quoted, sizeof(quoted), "\"%s\"", boot_id);
	} else {
		snprintf(quoted, sizeof(quoted), "null");
	}

	length = snprintf(out, size,
			  "{\"t_ms\":%" PRId64 ",\"src\":\"%s\",\"event\":\"open\",\"boot_id\":%s,"
			  "\"unix_ms\":%" PRId64 "}\n",
			  timestamp_ms, LOG_SOURCE_NAME, quoted, unix_ms);
	if (length < 0 || (size_t)length >= size) {
		return -ENOSPC;
	}

	return length;
}

int log_writer_start(struct log_writer *lw, struct ring_buffer *rb, const char *path)
{
	return log_writer_start_with(lw, rb, path, NULL);
}

int log_writer_start_with(struct log_writer *lw, struct ring_buffer *rb, const char *path,
			  const struct log_writer_options *options)
{
	int error;

	if (lw == NULL || rb == NULL || path == NULL) {
		return -EINVAL;
	}
	if (options != NULL && options->max_bytes > 0 &&
	    (options->keep < 1 || options->keep > LOG_WRITER_MAX_KEEP)) {
		return -EINVAL;
	}
	/* Запас на суфікс ".99" ротації. */
	if (strlen(path) + 4 >= sizeof(lw->path)) {
		return -ENAMETOOLONG;
	}

	error = init_detector(lw);
	if (error < 0) {
		return error;
	}

	strcpy(lw->path, path);
	lw->max_bytes = options != NULL ? options->max_bytes : 0;
	lw->keep = options != NULL ? options->keep : 0;
	lw->open_marker = options != NULL && options->open_marker;
	lw->dirty = false;
	lw->last_sync_ms = monotonic_ms();
	memset(&lw->stats, 0, sizeof(lw->stats));

	error = open_log(lw);
	if (error < 0) {
		return error;
	}

	lw->raw_fd = -1;
	if (options != NULL && options->raw_path != NULL) {
		error = open_raw_file(options->raw_path);
		if (error < 0) {
			close(lw->fd);
			return error;
		}
		lw->raw_fd = error;
	}

	lw->source_buffer = rb;
	lw->aux_seen = false;
	lw->last_aux_ms = 0;
	write_open_marker(lw);
	atomic_init(&lw->stop_requested, false);
	atomic_init(&lw->paused, false);
	atomic_init(&lw->needs_reset, false);
	atomic_init(&lw->moving, false);

	error = pthread_create(&lw->thread, NULL, run_writer_thread, lw);
	if (error != 0) {
		close(lw->fd);
		if (lw->raw_fd >= 0) {
			close(lw->raw_fd);
		}
		return -error;
	}

	return 0;
}

void log_writer_set_paused(struct log_writer *lw, bool paused)
{
	if (lw == NULL) {
		return;
	}
	if (!paused) {
		/* Спершу скидання, потім зняття паузи. Ознаку руху скидаємо одразу: поки записувач
		 * дійде до першого відліку, світлодіоди не повинні показувати рух до паузи. */
		atomic_store(&lw->moving, false);
		atomic_store(&lw->needs_reset, true);
	}
	atomic_store(&lw->paused, paused);
}

bool log_writer_is_moving(const struct log_writer *lw)
{
	return lw != NULL && atomic_load(&lw->moving);
}

int log_writer_stop(struct log_writer *lw, struct log_writer_stats *stats)
{
	int result = 0;

	if (lw == NULL) {
		return -EINVAL;
	}

	atomic_store(&lw->stop_requested, true);
	pthread_join(lw->thread, NULL);

	/* Після join потік уже не пише, тож підсумок можна читати без замка. */
	if (stats != NULL) {
		*stats = lw->stats;
	}

	sync_if_due(lw, true);
	if (lw->fd >= 0 && close(lw->fd) < 0) {
		result = -errno;
	}
	if (lw->raw_fd >= 0 && close(lw->raw_fd) < 0 && result == 0) {
		result = -errno;
	}

	return result;
}

/*
 * Готує фільтр руху з параметрами за замовчуванням (див. motion_defaults.h): при старті і після
 * паузи. Параметри допустимі за побудовою, тож помилка тут це помилка в них.
 */
static int init_detector(struct log_writer *lw)
{
	int error = motion_report_init(&lw->detector, MOTION_DEFAULT_WINDOW,
				       MOTION_DEFAULT_THRESHOLD_G, MOTION_DEFAULT_CONFIRM);

	if (error < 0) {
		return error;
	}

	return motion_report_set_release(&lw->detector, MOTION_DEFAULT_RELEASE_G);
}

/* Створює файл сирих відліків заново й пише в нього рядок-коментар із назвами стовпців. */
static int open_raw_file(const char *path)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, LOG_FILE_MODE);

	if (fd < 0) {
		return -errno;
	}
	if (write_all(fd, RAW_HEADER, strlen(RAW_HEADER)) < 0) {
		int error = -errno;

		close(fd);
		return error;
	}

	return fd;
}

/* Ім'я джерела потрапляє в JSON без екранування, тому дозволяємо лише безпечні символи. */
static bool is_valid_source(const char *source)
{
	size_t length;
	size_t i;

	if (source == NULL) {
		return false;
	}

	length = strlen(source);
	if (length == 0 || length >= LOG_WRITER_SOURCE_SIZE) {
		return false;
	}

	for (i = 0; i < length; i++) {
		char c = source[i];

		if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) {
			return false;
		}
	}

	return true;
}

static void *run_writer_thread(void *argument)
{
	struct log_writer *lw = argument;
	struct sample item;

	for (;;) {
		/*
		 * Прапорець читаємо ДО спроби забрати запис. Якщо зупинку вже запросили, виробники
		 * закінчили, і порожній буфер означає, що справді все дописано. Якби читали після
		 * pop, запис, покладений між pop і перевіркою, лишився б у буфері.
		 */
		bool stopping = atomic_load(&lw->stop_requested);

		if (ring_buffer_pop(lw->source_buffer, &item) == 0) {
			handle_sample(lw, &item);
			sync_if_due(lw, false);
			continue;
		}
		if (stopping) {
			break;
		}
		sync_if_due(lw, false);
		sleep_idle();
	}

	return NULL;
}

/* Розподіляє запис за джерелом: число імітатора пишемо одразу, відлік акселерометра йде у фільтр.
 */
static void handle_sample(struct log_writer *lw, const struct sample *item)
{
	if (atomic_load(&lw->paused)) {
		lw->stats.paused++;
		return;
	}
	if (atomic_exchange(&lw->needs_reset, false)) {
		(void)init_detector(lw);
		atomic_store(&lw->moving, false);
	}

	switch (item->source) {
	case SAMPLE_SOURCE_SIM:
		write_sim_sample(lw, item);
		break;
	case SAMPLE_SOURCE_ACCEL:
		write_accel_sample(lw, item);
		break;
	case SAMPLE_SOURCE_ACCEL2:
		write_aux_sample(lw, item);
		break;
	case SAMPLE_SOURCE_GPS:
		write_gps_sample(lw, item);
		break;
	default:
		lw->stats.failed++; /* невідоме джерело: краще порахувати, ніж мовчки загубити */
		break;
	}
}

/* Число імітатора: один рядок на число. */
static void write_sim_sample(struct log_writer *lw, const struct sample *item)
{
	char line[LOG_WRITER_LINE_SIZE];
	int length =
	    log_writer_format_line(line, sizeof(line), SIM_SOURCE_NAME, item->time_ms, item->value);

	write_line(lw, line, length);
}

/* Відлік основного акселерометра: у лог іде підсумок секунди й подія «рух почався / закінчився»,
 * сам відлік ні. */
static void write_accel_sample(struct log_writer *lw, const struct sample *item)
{
	struct accel_sample accel = {
	    .time_ms = item->time_ms, .x_g = item->x_g, .y_g = item->y_g, .z_g = item->z_g};
	struct motion_report_update update;
	char line[LOG_WRITER_LINE_SIZE];
	int length;

	write_raw_sample(lw, item);
	if (motion_report_add_sample(&lw->detector, &accel, &update) < 0) {
		lw->stats.failed++; /* час спадає або число не скінченне: відлік відкинуто */
		return;
	}
	if (update.row_ready) {
		length = log_writer_format_row(line, sizeof(line), item->time_ms, &update.row);
		write_line(lw, line, length);
		if (length >= 0) {
			lw->stats.rows++;
		}
	}
	if (update.event == MOTION_EVENT_NONE) {
		return;
	}

	atomic_store(&lw->moving, update.event == MOTION_EVENT_START);

	length = log_writer_format_event(line, sizeof(line), item->time_ms, update.event);
	write_line(lw, line, length);
	if (length >= 0) {
		lw->stats.events++;
	}
}

/* Додатковий акселерометр: не частіше одного рядка на AUX_PERIOD_MS за мітками часу відліків. */
static void write_aux_sample(struct log_writer *lw, const struct sample *item)
{
	char line[LOG_WRITER_LINE_SIZE];
	int length;

	if (lw->aux_seen && item->time_ms - lw->last_aux_ms < AUX_PERIOD_MS) {
		return;
	}

	length = log_writer_format_aux(line, sizeof(line), item->time_ms, item->x_g, item->y_g,
				       item->z_g);
	write_line(lw, line, length);
	if (length >= 0) {
		lw->stats.aux++;
		lw->aux_seen = true;
		lw->last_aux_ms = item->time_ms;
	}
}

/* GPS: один рядок на кожен рядок RMC від модуля. */
static void write_gps_sample(struct log_writer *lw, const struct sample *item)
{
	char line[LOG_WRITER_LINE_SIZE];
	int length = log_writer_format_gps(line, sizeof(line), item);

	write_line(lw, line, length);
	if (length >= 0) {
		lw->stats.gps++;
	}
}

/* Записує готовий рядок одним викликом (length < 0 це помилка форматування); рахує результат. */
static void write_line(struct log_writer *lw, const char *line, int length)
{
	if (length < 0 || lw->fd < 0 || write_all(lw->fd, line, (size_t)length) < 0) {
		lw->stats.failed++;
		return;
	}

	lw->stats.written++;
	lw->size += (uint64_t)length;
	lw->dirty = true;
	if (lw->max_bytes > 0 && lw->size >= lw->max_bytes) {
		rotate(lw);
	}
}

/* Рядок "src":"log","event":"open": після кожного відкриття файлу, якщо його просили. */
static void write_open_marker(struct log_writer *lw)
{
	char boot_id[BOOT_ID_LENGTH + 1];
	char line[LOG_WRITER_LINE_SIZE];
	int length;

	if (!lw->open_marker) {
		return;
	}
	read_boot_id(boot_id, sizeof(boot_id));
	length = log_writer_format_open(line, sizeof(line), monotonic_ms(),
					boot_id[0] != '\0' ? boot_id : NULL, realtime_ms());
	write_line(lw, line, length);
}

/* Відкриває lw->path на дописування і запам'ятовує, скільки в ньому вже є. */
static int open_log(struct log_writer *lw)
{
	struct stat info;

	lw->fd = open(lw->path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, LOG_FILE_MODE);
	if (lw->fd < 0) {
		return -errno;
	}
	lw->size = fstat(lw->fd, &info) == 0 ? (uint64_t)info.st_size : 0;

	return 0;
}

/*
 * Ротація: path.(keep-1) -> path.keep, …, path -> path.1, новий порожній path. Якщо щось не
 * вдалося перейменувати, пишемо далі в той файл, що відкрився: краще задовгий лог, ніж ніякого.
 */
static void rotate(struct log_writer *lw)
{
	char from[LOG_WRITER_PATH_SIZE + 16];
	char to[LOG_WRITER_PATH_SIZE + 16];
	unsigned int i;

	sync_if_due(lw, true);
	close(lw->fd);
	lw->fd = -1;

	for (i = lw->keep; i > 1; i--) {
		snprintf(from, sizeof(from), "%s.%u", lw->path, i - 1);
		snprintf(to, sizeof(to), "%s.%u", lw->path, i);
		if (rename(from, to) < 0 && errno != ENOENT) {
			lw->stats.failed++;
		}
	}
	snprintf(to, sizeof(to), "%s.1", lw->path);
	if (rename(lw->path, to) < 0) {
		lw->stats.failed++;
	}

	if (open_log(lw) < 0) {
		lw->stats.failed++; /* наступні рядки порахуються як помилки, програма не впаде */
		return;
	}
	lw->stats.rotations++;
	write_open_marker(lw);
}

/* fdatasync, якщо є нескинуте й минув інтервал (або force). */
static void sync_if_due(struct log_writer *lw, bool force)
{
	int64_t now = monotonic_ms();

	if (!lw->dirty || lw->fd < 0) {
		return;
	}
	if (!force && now - lw->last_sync_ms < LOG_WRITER_SYNC_INTERVAL_MS) {
		return;
	}
	if (fdatasync(lw->fd) < 0) {
		lw->stats.failed++;
	}
	lw->dirty = false;
	lw->last_sync_ms = now;
}

/* UUID з малих шістнадцяткових цифр і дефісів: іде в JSON без екранування. */
static bool is_valid_boot_id(const char *boot_id)
{
	size_t i;

	if (strlen(boot_id) != BOOT_ID_LENGTH) {
		return false;
	}
	for (i = 0; i < BOOT_ID_LENGTH; i++) {
		char c = boot_id[i];

		if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || c == '-')) {
			return false;
		}
	}

	return true;
}

/* Читає boot_id; порожній рядок, якщо файла нема чи вміст дивний (тоді в лозі null). */
static void read_boot_id(char *out, size_t size)
{
	int fd = open(BOOT_ID_PATH, O_RDONLY | O_CLOEXEC);
	ssize_t length;

	out[0] = '\0';
	if (fd < 0) {
		return;
	}
	length = read(fd, out, size - 1);
	close(fd);
	if (length <= 0) {
		out[0] = '\0';
		return;
	}
	out[length] = '\0';
	out[strcspn(out, "\n")] = '\0';
	if (!is_valid_boot_id(out)) {
		out[0] = '\0';
	}
}

static int64_t realtime_ms(void)
{
	struct timespec now;

	clock_gettime(CLOCK_REALTIME, &now);

	return (int64_t)now.tv_sec * MSEC_PER_SEC + now.tv_nsec / NSEC_PER_MSEC;
}

/* Сирий відлік основного акселерометра в файл сирих відліків (якщо його задано), формат accel_csv.
 */
static void write_raw_sample(struct log_writer *lw, const struct sample *item)
{
	char line[ACCEL_CSV_MAX_LINE];
	int length;

	if (lw->raw_fd < 0) {
		return;
	}

	length = snprintf(line, sizeof(line), "%" PRId64 ",%.5f,%.5f,%.5f\n", item->time_ms,
			  item->x_g, item->y_g, item->z_g);
	if (length < 0 || (size_t)length >= sizeof(line) ||
	    write_all(lw->raw_fd, line, (size_t)length) < 0) {
		lw->stats.failed++;
	}
}

static void sleep_idle(void)
{
	struct timespec pause = {.tv_sec = 0, .tv_nsec = IDLE_SLEEP_MS * NSEC_PER_MSEC};

	nanosleep(&pause, NULL);
}
