/**
 * @file main.c
 * @brief Точка входу motion_logger: бере відліки з джерел, кладе в буфер, пише лог.
 *
 * Основне джерело акселерометра (не більше одного з трьох):
 *   --stdin              числа від імітатора зі стандартного вводу:
 *                          ./build/sensor_sim | ./build/motion_logger --stdin --log motion.jsonl
 *   --replay <файл>      «програвач» CSV-запису (формат t_ms,x_g,y_g,z_g); до нього --speed
 * <число>: 1 реальний час (за замовчуванням), 50 у 50 разів швидше, 0 без пауз
 *   --iio <каталог>      справжній акселерометр через IIO sysfs, наприклад
 *                          /sys/bus/iio/devices/iio:device0 (MPU6050)
 * Додаткові джерела (можна разом з основним і між собою):
 *   --iio-aux <каталог>  другий акселерометр (ADXL345), у лог раз на секунду
 *   --iio-gyro <каталог> гіроскоп і температура MPU6050 (той самий каталог, що й --iio), раз на
 * секунду
 *   --iio-rate <Гц>      частота опитування IIO, 1..1000 (за замовчуванням 100)
 *   --gps <порт>         GPS по послідовному порту, наприклад /dev/ttyAMA2
 *   --gps-baud <бод>     швидкість порту (за замовчуванням 9600)
 * Результат:
 *   --log <файл>         JSON Lines (див. log_writer.h); без нього відліки лише збираються в буфері
 *
 * Без жодного джерела програма нічого не читає й одразу завершується успішно: так її запускає
 * скрипт автозапуску образу, коли в /etc/default/motion_logger параметрів нема.
 *
 * Кожне джерело працює у власному потоці й кладе записи в спільний кільцевий буфер (він
 * потокобезпечний), а один потік-записувач розкладає їх у лог. Скінченні джерела (--stdin,
 * --replay) закінчуються самі, решта працюють до SIGINT/SIGTERM.
 *
 * Це єдине місце, яке знає і про ядро (core/), і про драйвери (drivers/): ядро працює лише з
 * інтерфейсом struct sensor і дескрипторами.
 */
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "csv_player.h"
#include "gps_reader.h"
#include "iio_accel.h"
#include "log_writer.h"
#include "ring_buffer.h"
#include "sample_reader.h"
#include "sensor_reader.h"
#include "uart_port.h"

#define BUFFER_CAPACITY 4096
#define MAX_TASKS 4

#define STDIN_OPTION "--stdin"
#define REPLAY_OPTION "--replay"
#define SPEED_OPTION "--speed"
#define IIO_OPTION "--iio"
#define IIO_AUX_OPTION "--iio-aux"
#define IIO_GYRO_OPTION "--iio-gyro"
#define IIO_RATE_OPTION "--iio-rate"
#define GPS_OPTION "--gps"
#define GPS_BAUD_OPTION "--gps-baud"
#define LOG_OPTION "--log"

#define REALTIME_SPEED 1.0
#define DEFAULT_IIO_RATE_HZ 100
#define DEFAULT_GPS_BAUD 9600

/* Параметри запуску, розібрані з командного рядка. */
struct options {
	bool read_stdin;
	const char *replay_path; /* NULL, якщо --replay не задано */
	double replay_speed;
	const char *iio_path;	   /* основний акселерометр, або NULL */
	const char *iio_aux_path;  /* додатковий акселерометр, або NULL */
	const char *iio_gyro_path; /* гіроскоп (і температура) пристрою IIO, або NULL */
	unsigned int iio_rate_hz;
	const char *gps_path; /* NULL, якщо --gps не задано */
	unsigned int gps_baud;
	const char *log_path; /* NULL, якщо --log не задано */
};

/* Скільки записів прийшло від джерела і скільки з них не влізло в буфер. */
struct source_stats {
	size_t received;
	size_t dropped;
};

/* Одне джерело: відкритий ресурс, функція потоку і результат. */
struct task {
	const char *name; /* для повідомлень про помилки */
	int (*run)(struct task *task);
	struct ring_buffer *buffer;
	int fd;			   /* дескриптор (stdin чи GPS), або -1 */
	struct sensor *sensor;	   /* датчик (replay чи IIO), або NULL */
	enum sample_source source; /* яким джерелом позначати записи датчика */
	const unsigned long
	    *line_number; /* номер рядка CSV для повідомлення про помилку, або NULL */
	pthread_t thread;
	bool started;
	int result;
	struct source_stats stats;
};

/* Стає 1, коли користувач натиснув Ctrl+C або init просить зупинитись (SIGTERM). */
static volatile sig_atomic_t stop_requested;

static int parse_options(int argc, char **argv, struct options *options);
static int parse_speed(const char *text, double *speed);
static int parse_unsigned(const char *text, unsigned int *value);
static void print_usage(const char *program);
static void handle_stop_signal(int signal_number);
static int install_stop_handlers(void);
static bool has_any_source(const struct options *options);
static int run_sources(const struct options *options, struct ring_buffer *buffer,
		       struct source_stats *total);
static void *task_thread(void *argument);
static int run_stdin_task(struct task *task);
static int run_sensor_task(struct task *task);
static int run_gps_task(struct task *task);

int main(int argc, char **argv)
{
	static struct sample storage[BUFFER_CAPACITY];
	struct ring_buffer buffer;
	struct source_stats source_stats = {0};
	struct log_writer_stats log_stats = {0};
	struct log_writer writer;
	struct options options;
	bool writer_started = false;
	int result;

	if (parse_options(argc, argv, &options) < 0) {
		print_usage(argv[0]);
		return EXIT_FAILURE;
	}
	if (!has_any_source(&options)) {
		fprintf(stderr, "motion_logger: no sample source (use %s, %s, %s, %s, %s or %s)\n",
			STDIN_OPTION, REPLAY_OPTION, IIO_OPTION, IIO_AUX_OPTION, IIO_GYRO_OPTION,
			GPS_OPTION);
		return EXIT_SUCCESS;
	}

	result = ring_buffer_init(&buffer, storage, BUFFER_CAPACITY);
	if (result < 0) {
		fprintf(stderr, "motion_logger: ring buffer: %s\n", strerror(-result));
		return EXIT_FAILURE;
	}

	result = install_stop_handlers();
	if (result < 0) {
		fprintf(stderr, "motion_logger: signals: %s\n", strerror(-result));
		goto cleanup;
	}

	if (options.log_path != NULL) {
		result = log_writer_start(&writer, &buffer, options.log_path);
		if (result < 0) {
			fprintf(stderr, "motion_logger: log %s: %s\n", options.log_path,
				strerror(-result));
			goto cleanup;
		}
		writer_started = true;
	}

	result = run_sources(&options, &buffer, &source_stats);

	/* Джерела закінчили, нових записів не буде: тепер записувач може дописати залишок. */
	if (writer_started) {
		int stop_result = log_writer_stop(&writer, &log_stats);

		if (stop_result < 0) {
			fprintf(stderr, "motion_logger: log close: %s\n", strerror(-stop_result));
			result = result < 0 ? result : stop_result;
		}
	}

	printf("received %zu, dropped %zu, logged %zu (events %zu, rows %zu, aux %zu, gyro %zu, "
	       "gps %zu), log errors %zu, in buffer %zu\n",
	       source_stats.received, source_stats.dropped, log_stats.written, log_stats.events,
	       log_stats.rows, log_stats.aux, log_stats.gyro, log_stats.gps, log_stats.failed,
	       ring_buffer_count(&buffer));

cleanup:
	ring_buffer_destroy(&buffer);
	return result < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}

/*
 * Розбирає аргументи; повертає 0, або -EINVAL, якщо аргумент невідомий, бракує значення,
 * основних джерел кілька або параметр джерела заданий без самого джерела.
 */
static int parse_options(int argc, char **argv, struct options *options)
{
	bool speed_given = false;
	bool rate_given = false;
	bool baud_given = false;
	int primary_sources;
	int i;

	memset(options, 0, sizeof(*options));
	options->replay_speed = REALTIME_SPEED;
	options->iio_rate_hz = DEFAULT_IIO_RATE_HZ;
	options->gps_baud = DEFAULT_GPS_BAUD;

	for (i = 1; i < argc; i++) {
		bool has_value = i + 1 < argc;

		if (strcmp(argv[i], STDIN_OPTION) == 0) {
			options->read_stdin = true;
		} else if (has_value && strcmp(argv[i], REPLAY_OPTION) == 0) {
			options->replay_path = argv[++i];
		} else if (has_value && strcmp(argv[i], SPEED_OPTION) == 0) {
			if (parse_speed(argv[++i], &options->replay_speed) < 0) {
				return -EINVAL;
			}
			speed_given = true;
		} else if (has_value && strcmp(argv[i], IIO_OPTION) == 0) {
			options->iio_path = argv[++i];
		} else if (has_value && strcmp(argv[i], IIO_AUX_OPTION) == 0) {
			options->iio_aux_path = argv[++i];
		} else if (has_value && strcmp(argv[i], IIO_GYRO_OPTION) == 0) {
			options->iio_gyro_path = argv[++i];
		} else if (has_value && strcmp(argv[i], IIO_RATE_OPTION) == 0) {
			if (parse_unsigned(argv[++i], &options->iio_rate_hz) < 0) {
				return -EINVAL;
			}
			rate_given = true;
		} else if (has_value && strcmp(argv[i], GPS_OPTION) == 0) {
			options->gps_path = argv[++i];
		} else if (has_value && strcmp(argv[i], GPS_BAUD_OPTION) == 0) {
			if (parse_unsigned(argv[++i], &options->gps_baud) < 0) {
				return -EINVAL;
			}
			baud_given = true;
		} else if (has_value && strcmp(argv[i], LOG_OPTION) == 0) {
			options->log_path = argv[++i];
		} else {
			return -EINVAL;
		}
	}

	primary_sources = (options->read_stdin ? 1 : 0) + (options->replay_path != NULL ? 1 : 0) +
			  (options->iio_path != NULL ? 1 : 0);
	if (primary_sources > 1) {
		return -EINVAL; /* основне джерело акселерометра одне */
	}
	if (speed_given && options->replay_path == NULL) {
		return -EINVAL; /* швидкість має зміст лише для --replay */
	}
	if (rate_given && options->iio_path == NULL && options->iio_aux_path == NULL &&
	    options->iio_gyro_path == NULL) {
		return -EINVAL;
	}
	if (baud_given && options->gps_path == NULL) {
		return -EINVAL;
	}

	return 0;
}

/* Розбирає швидкість програвання: невід'ємне десяткове число, увесь текст. */
static int parse_speed(const char *text, double *speed)
{
	char *end;
	double value;

	errno = 0;
	value = strtod(text, &end);
	if (errno != 0 || end == text || *end != '\0' || !(value >= 0)) {
		return -EINVAL;
	}
	*speed = value;

	return 0;
}

/* Розбирає додатне ціле число (частота, швидкість порту): увесь текст, без знаку. */
static int parse_unsigned(const char *text, unsigned int *value)
{
	char *end;
	unsigned long parsed;

	if (text[0] < '0' || text[0] > '9') {
		return -EINVAL;
	}
	errno = 0;
	parsed = strtoul(text, &end, 10);
	if (errno != 0 || *end != '\0' || parsed == 0 || parsed > 4000000UL) {
		return -EINVAL;
	}
	*value = (unsigned int)parsed;

	return 0;
}

static void print_usage(const char *program)
{
	fprintf(stderr,
		"usage: %s [%s | %s file [%s factor] | %s dir [%s hz]] [%s dir] [%s dir] "
		"[%s port [%s baud]] [%s file]\n",
		program, STDIN_OPTION, REPLAY_OPTION, SPEED_OPTION, IIO_OPTION, IIO_RATE_OPTION,
		IIO_AUX_OPTION, IIO_GYRO_OPTION, GPS_OPTION, GPS_BAUD_OPTION, LOG_OPTION);
}

static bool has_any_source(const struct options *options)
{
	return options->read_stdin || options->replay_path != NULL || options->iio_path != NULL ||
	       options->iio_aux_path != NULL || options->iio_gyro_path != NULL ||
	       options->gps_path != NULL;
}

/* У обробнику сигналу можна лише виставити прапорець: усе інше в ньому небезпечно. */
static void handle_stop_signal(int signal_number)
{
	(void)signal_number;
	stop_requested = 1;
}

static int install_stop_handlers(void)
{
	struct sigaction action = {.sa_handler = handle_stop_signal};

	sigemptyset(&action.sa_mask);
	if (sigaction(SIGINT, &action, NULL) < 0 || sigaction(SIGTERM, &action, NULL) < 0) {
		return -errno;
	}

	return 0;
}

/*
 * Відкриває всі джерела (помилка відкриття видна одразу, до запуску потоків), запускає потік на
 * кожне, чекає їх і закриває ресурси. Єдине місце, де драйвери підставляються для ядра.
 */
static int run_sources(const struct options *options, struct ring_buffer *buffer,
		       struct source_stats *total)
{
	struct task tasks[MAX_TASKS];
	struct csv_player player;
	struct iio_accel primary_iio;
	struct iio_accel aux_iio;
	struct iio_accel gyro_iio;
	size_t count = 0;
	size_t i;
	int result = 0;
	bool player_open = false;
	bool primary_iio_open = false;
	bool aux_iio_open = false;
	bool gyro_iio_open = false;
	int gps_fd = -1;

	memset(tasks, 0, sizeof(tasks));
	for (i = 0; i < MAX_TASKS; i++) {
		tasks[i].fd = -1;
		tasks[i].buffer = buffer;
	}

	if (options->read_stdin) {
		tasks[count].name = "stdin";
		tasks[count].run = run_stdin_task;
		tasks[count].fd = STDIN_FILENO;
		count++;
	}
	if (options->replay_path != NULL) {
		result = csv_player_open(&player, options->replay_path, options->replay_speed);
		if (result < 0) {
			fprintf(stderr, "motion_logger: replay %s: %s\n", options->replay_path,
				strerror(-result));
			goto cleanup;
		}
		player_open = true;
		tasks[count].name = "replay";
		tasks[count].run = run_sensor_task;
		tasks[count].sensor = &player.base;
		tasks[count].source = SAMPLE_SOURCE_ACCEL;
		tasks[count].line_number = &player.line_number;
		count++;
	}
	if (options->iio_path != NULL) {
		result = iio_accel_open(&primary_iio, options->iio_path, options->iio_rate_hz);
		if (result < 0) {
			fprintf(stderr, "motion_logger: iio %s: %s\n", options->iio_path,
				strerror(-result));
			goto cleanup;
		}
		primary_iio_open = true;
		tasks[count].name = "iio";
		tasks[count].run = run_sensor_task;
		tasks[count].sensor = &primary_iio.base;
		tasks[count].source = SAMPLE_SOURCE_ACCEL;
		count++;
	}
	if (options->iio_aux_path != NULL) {
		result = iio_accel_open(&aux_iio, options->iio_aux_path, options->iio_rate_hz);
		if (result < 0) {
			fprintf(stderr, "motion_logger: iio-aux %s: %s\n", options->iio_aux_path,
				strerror(-result));
			goto cleanup;
		}
		aux_iio_open = true;
		tasks[count].name = "iio-aux";
		tasks[count].run = run_sensor_task;
		tasks[count].sensor = &aux_iio.base;
		tasks[count].source = SAMPLE_SOURCE_ACCEL2;
		count++;
	}
	if (options->iio_gyro_path != NULL) {
		result =
		    iio_accel_open_gyro(&gyro_iio, options->iio_gyro_path, options->iio_rate_hz);
		if (result < 0) {
			fprintf(stderr, "motion_logger: iio-gyro %s: %s\n", options->iio_gyro_path,
				strerror(-result));
			goto cleanup;
		}
		gyro_iio_open = true;
		tasks[count].name = "iio-gyro";
		tasks[count].run = run_sensor_task;
		tasks[count].sensor = &gyro_iio.base;
		tasks[count].source = SAMPLE_SOURCE_GYRO;
		count++;
	}
	if (options->gps_path != NULL) {
		gps_fd = uart_port_open(options->gps_path, options->gps_baud);
		if (gps_fd < 0) {
			fprintf(stderr, "motion_logger: gps %s: %s\n", options->gps_path,
				strerror(-gps_fd));
			result = gps_fd;
			gps_fd = -1;
			goto cleanup;
		}
		tasks[count].name = "gps";
		tasks[count].run = run_gps_task;
		tasks[count].fd = gps_fd;
		count++;
	}

	for (i = 0; i < count; i++) {
		int error = pthread_create(&tasks[i].thread, NULL, task_thread, &tasks[i]);

		if (error != 0) {
			fprintf(stderr, "motion_logger: thread %s: %s\n", tasks[i].name,
				strerror(error));
			stop_requested = 1; /* уже запущені потоки завершаться самі */
			result = -error;
			break;
		}
		tasks[i].started = true;
	}

	for (i = 0; i < count; i++) {
		if (!tasks[i].started) {
			continue;
		}
		pthread_join(tasks[i].thread, NULL);
		total->received += tasks[i].stats.received;
		total->dropped += tasks[i].stats.dropped;
		if (tasks[i].result < 0 && result == 0) {
			result = tasks[i].result;
		}
	}

cleanup:
	if (player_open) {
		player.base.close(&player.base);
	}
	if (primary_iio_open) {
		primary_iio.base.close(&primary_iio.base);
	}
	if (aux_iio_open) {
		aux_iio.base.close(&aux_iio.base);
	}
	if (gyro_iio_open) {
		gyro_iio.base.close(&gyro_iio.base);
	}
	if (gps_fd >= 0) {
		close(gps_fd);
	}

	return result;
}

/* Тіло потоку: виконує джерело; помилка джерела зупиняє й решту, щоб програма завершилась акуратно.
 */
static void *task_thread(void *argument)
{
	struct task *task = argument;

	task->result = task->run(task);
	if (task->result < 0) {
		if (task->line_number != NULL) {
			fprintf(stderr, "motion_logger: %s line %lu: %s\n", task->name,
				*task->line_number, strerror(-task->result));
		} else {
			fprintf(stderr, "motion_logger: %s: %s\n", task->name,
				strerror(-task->result));
		}
		stop_requested = 1;
	}

	return NULL;
}

static int run_stdin_task(struct task *task)
{
	struct sample_reader_stats stats;
	int result = sample_reader_run(task->fd, task->buffer, &stop_requested, &stats);

	task->stats.received = stats.received;
	task->stats.dropped = stats.dropped;

	return result;
}

/* Датчик (CSV чи IIO) через інтерфейс struct sensor: ядро не знає, що за ним стоїть. */
static int run_sensor_task(struct task *task)
{
	struct sensor_reader_stats stats;
	int result =
	    sensor_reader_run(task->sensor, task->source, task->buffer, &stop_requested, &stats);

	task->stats.received = stats.received;
	task->stats.dropped = stats.dropped;

	return result;
}

static int run_gps_task(struct task *task)
{
	struct gps_reader_stats stats;
	int result = gps_reader_run(task->fd, task->buffer, &stop_requested, &stats);

	task->stats.received = stats.samples;
	task->stats.dropped = stats.dropped;

	return result;
}
