/**
 * @file main.c
 * @brief Точка входу motion_logger: бере відліки з джерел, кладе в буфер, пише лог.
 *
 * Основне джерело акселерометра (не більше одного з трьох):
 *   --stdin              числа від імітатора зі стандартного вводу:
 *                          ./build/sensor_sim | ./build/motion_logger --stdin --log motion.jsonl
 *   --replay <файл>      «програвач» CSV-запису (формат t_ms,x_g,y_g,z_g); до нього --speed
 * <число>: 1 реальний час (за замовчуванням), 50 у 50 разів швидше, 0 без пауз
 *   --iio <пристрій>     справжній акселерометр через IIO sysfs: каталог, наприклад
 *                          /sys/bus/iio/devices/iio:device0, або ім'я драйвера (mpu6050)
 * Додаткові джерела (можна разом з основним і між собою):
 *   --iio-aux <пристрій> другий акселерометр (ADXL345), у лог раз на секунду
 *   --iio-rate <Гц>      частота опитування IIO, 1..1000 (за замовчуванням 100)
 *   --gps <порт>         GPS по послідовному порту, наприклад /dev/ttyAMA2
 *   --gps-baud <бод>     швидкість порту (за замовчуванням 9600)
 *   --gpio <чип>         світлодіоди стану й кнопки плати, наприклад /dev/gpiochip0 (потрібен
 * --log)
 *   --button-active-high кнопка дає 3,3 В при натисканні (за замовчуванням замикає на землю)
 * Результат:
 *   --log <файл>         JSON Lines (див. log_writer.h); без нього відліки лише збираються в буфері
 *   --log-max-mb <МБ>    ротація: новий файл, коли лог досяг розміру; старі <файл>.1 … .N
 *   --log-keep <N>       скільки старих файлів лишати, 1..99 (за замовчуванням 5)
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

#include <sys/epoll.h>

#include "csv_player.h"
#include "device_status.h"
#include "gpio_lines.h"
#include "gps_reader.h"
#include "iio_accel.h"
#include "io_util.h"
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
#define IIO_RATE_OPTION "--iio-rate"
#define GPS_OPTION "--gps"
#define GPS_BAUD_OPTION "--gps-baud"
#define LOG_OPTION "--log"
#define LOG_MAX_MB_OPTION "--log-max-mb"
#define LOG_KEEP_OPTION "--log-keep"
#define GPIO_OPTION "--gpio"
#define BUTTON_ACTIVE_HIGH_OPTION "--button-active-high"

#define REALTIME_SPEED 1.0
#define DEFAULT_IIO_RATE_HZ 100
#define DEFAULT_GPS_BAUD 9600
#define DEFAULT_LOG_KEEP 5
#define MAX_LOG_MB 100000
#define BYTES_PER_MB (1024ULL * 1024ULL)

/* Плата Motion Logger (README, «Схема підключення»): номери GPIO на гребінці Pi 5. */
#define LED_RED_GPIO 27
#define LED_GREEN_GPIO 22
#define LED_BLUE_GPIO 6
#define BUTTON1_GPIO 17
#define BUTTON2_GPIO 13
#define LED_COUNT 3
#define BUTTON_COUNT 2
#define LED_RED_BIT (1U << 0)
#define LED_GREEN_BIT (1U << 1)
#define LED_BLUE_BIT (1U << 2)
#define BUTTON_DEBOUNCE_US 30000
/* Як часто оновлюються світлодіоди: досить для блимання 2 Гц. */
#define LED_TICK_MS 50
#define GPIO_CONSUMER "motion_logger"
#define IIO_PATH_SIZE 256

/* Параметри запуску, розібрані з командного рядка. */
struct options {
	bool read_stdin;
	const char *replay_path; /* NULL, якщо --replay не задано */
	double replay_speed;
	const char *iio_path;	  /* основний акселерометр, або NULL */
	const char *iio_aux_path; /* додатковий акселерометр, або NULL */
	unsigned int iio_rate_hz;
	const char *gps_path; /* NULL, якщо --gps не задано */
	unsigned int gps_baud;
	const char *log_path;	 /* NULL, якщо --log не задано */
	unsigned int log_max_mb; /* 0: без ротації */
	unsigned int log_keep;
	const char *gpio_chip; /* NULL, якщо --gpio не задано */
	bool button_active_high;
	char iio_resolved[IIO_PATH_SIZE];     /* каталог для --iio, заданого іменем */
	char iio_aux_resolved[IIO_PATH_SIZE]; /* те саме для --iio-aux */
};

/* Скільки записів прийшло від джерела і скільки з них не влізло в буфер. */
struct source_stats {
	size_t received;
	size_t dropped;
	size_t errors; /* тимчасові збої читання датчика (відлік пропущено) */
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
	    *line_number;	      /* номер рядка CSV для повідомлення про помилку, або NULL */
	struct device_status *status; /* для світлодіодів (лише задача GPIO) */
	int led_fd;		      /* лінії світлодіодів, або -1 */
	int button_fd;		      /* лінії кнопок, або -1 */
	pthread_t thread;
	bool started;
	int result;
	struct source_stats stats;
};

/* Стає 1, коли користувач натиснув Ctrl+C або init просить зупинитись (SIGTERM). */
static volatile sig_atomic_t stop_requested;

/* Стає 1, коли всі джерела даних завершились: тоді й світлодіодам більше нічого показувати. */
static volatile sig_atomic_t sources_finished;

static int parse_options(int argc, char **argv, struct options *options);
static int parse_speed(const char *text, double *speed);
static int parse_unsigned(const char *text, unsigned int *value);
static int resolve_iio(const char **path, char *resolved, const char *option);
static void print_usage(const char *program);
static void handle_stop_signal(int signal_number);
static int install_stop_handlers(void);
static bool has_any_source(const struct options *options);
static int run_sources(const struct options *options, struct ring_buffer *buffer,
		       struct device_status *status, struct source_stats *total);
static void *task_thread(void *argument);
static int run_stdin_task(struct task *task);
static int run_sensor_task(struct task *task);
static int run_gps_task(struct task *task);
static int run_gpio_task(struct task *task);
static void show_leds(int led_fd, const struct status_leds *leds);

int main(int argc, char **argv)
{
	static struct sample storage[BUFFER_CAPACITY];
	struct ring_buffer buffer;
	struct source_stats source_stats = {0};
	struct log_writer_stats log_stats = {0};
	struct log_writer writer;
	struct device_status status;
	struct options options;
	bool writer_started = false;
	int result;

	if (parse_options(argc, argv, &options) < 0) {
		print_usage(argv[0]);
		return EXIT_FAILURE;
	}
	if (resolve_iio(&options.iio_path, options.iio_resolved, IIO_OPTION) < 0 ||
	    resolve_iio(&options.iio_aux_path, options.iio_aux_resolved, IIO_AUX_OPTION) < 0) {
		return EXIT_FAILURE;
	}
	if (!has_any_source(&options)) {
		fprintf(stderr, "motion_logger: no sample source (use %s, %s, %s, %s or %s)\n",
			STDIN_OPTION, REPLAY_OPTION, IIO_OPTION, IIO_AUX_OPTION, GPS_OPTION);
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

	/* --stdin дає числа імітатора, а не відліки акселерометра: за ними світлодіоди не стежать.
	 */
	device_status_init(&status, options.replay_path != NULL || options.iio_path != NULL,
			   options.gps_path != NULL, monotonic_ms());

	if (options.log_path != NULL) {
		struct log_writer_config config = {
		    .path = options.log_path,
		    .max_bytes = (uint64_t)options.log_max_mb * BYTES_PER_MB,
		    .keep = options.log_keep,
		    .open_marker = true,
		    .status = &status,
		};

		result = log_writer_start_config(&writer, &buffer, &config);
		if (result < 0) {
			fprintf(stderr, "motion_logger: log %s: %s\n", options.log_path,
				strerror(-result));
			goto cleanup;
		}
		writer_started = true;
	}

	result = run_sources(&options, &buffer, &status, &source_stats);

	/* Джерела закінчили, нових записів не буде: тепер записувач може дописати залишок. */
	if (writer_started) {
		int stop_result = log_writer_stop(&writer, &log_stats);

		if (stop_result < 0) {
			fprintf(stderr, "motion_logger: log close: %s\n", strerror(-stop_result));
			result = result < 0 ? result : stop_result;
		}
	}

	printf("received %zu, dropped %zu, read errors %zu, logged %zu (events %zu, rows %zu, "
	       "aux %zu, gps %zu, buttons %zu), log errors %zu, rotations %zu, in buffer %zu\n",
	       source_stats.received, source_stats.dropped, source_stats.errors, log_stats.written,
	       log_stats.events, log_stats.rows, log_stats.aux, log_stats.gps, log_stats.buttons,
	       log_stats.failed, log_stats.rotations, ring_buffer_count(&buffer));

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
	bool keep_given = false;
	int primary_sources;
	int i;

	memset(options, 0, sizeof(*options));
	options->replay_speed = REALTIME_SPEED;
	options->iio_rate_hz = DEFAULT_IIO_RATE_HZ;
	options->gps_baud = DEFAULT_GPS_BAUD;
	options->log_keep = DEFAULT_LOG_KEEP;

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
		} else if (has_value && strcmp(argv[i], LOG_MAX_MB_OPTION) == 0) {
			if (parse_unsigned(argv[++i], &options->log_max_mb) < 0 ||
			    options->log_max_mb > MAX_LOG_MB) {
				return -EINVAL;
			}
		} else if (has_value && strcmp(argv[i], LOG_KEEP_OPTION) == 0) {
			if (parse_unsigned(argv[++i], &options->log_keep) < 0 ||
			    options->log_keep > LOG_WRITER_MAX_KEEP) {
				return -EINVAL;
			}
			keep_given = true;
		} else if (has_value && strcmp(argv[i], GPIO_OPTION) == 0) {
			options->gpio_chip = argv[++i];
		} else if (strcmp(argv[i], BUTTON_ACTIVE_HIGH_OPTION) == 0) {
			options->button_active_high = true;
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
	if (rate_given && options->iio_path == NULL && options->iio_aux_path == NULL) {
		return -EINVAL;
	}
	if (baud_given && options->gps_path == NULL) {
		return -EINVAL;
	}
	if ((options->log_max_mb > 0 || keep_given) && options->log_path == NULL) {
		return -EINVAL;
	}
	if (keep_given && options->log_max_mb == 0) {
		return -EINVAL; /* старі файли бувають лише з ротацією */
	}
	/* Світлодіоди показують стан, який веде записувач логу. */
	if (options->gpio_chip != NULL && options->log_path == NULL) {
		return -EINVAL;
	}
	if (options->button_active_high && options->gpio_chip == NULL) {
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

/*
 * --iio mpu6050: значення без '/' це ім'я пристрою IIO, шукаємо його каталог. Шлях лишаємо як є.
 */
static int resolve_iio(const char **path, char *resolved, const char *option)
{
	int result;

	if (*path == NULL || strchr(*path, '/') != NULL) {
		return 0;
	}
	result = iio_accel_find(IIO_ACCEL_DEVICES_DIR, *path, resolved, IIO_PATH_SIZE);
	if (result == -ENODEV) {
		fprintf(stderr, "motion_logger: %s %s: no IIO device with this name in %s\n",
			option, *path, IIO_ACCEL_DEVICES_DIR);
		return result;
	}
	if (result == -ENOTUNIQ) {
		fprintf(stderr,
			"motion_logger: %s %s: several IIO devices with this name, give the path\n",
			option, *path);
		return result;
	}
	if (result < 0) {
		fprintf(stderr, "motion_logger: %s %s: %s\n", option, *path, strerror(-result));
		return result;
	}
	*path = resolved;

	return 0;
}

static void print_usage(const char *program)
{
	fprintf(stderr,
		"usage: %s [%s | %s file [%s factor] | %s dir|name [%s hz]] [%s dir|name] "
		"[%s port [%s baud]] [%s file [%s mb [%s n]]] [%s chip [%s]]\n",
		program, STDIN_OPTION, REPLAY_OPTION, SPEED_OPTION, IIO_OPTION, IIO_RATE_OPTION,
		IIO_AUX_OPTION, GPS_OPTION, GPS_BAUD_OPTION, LOG_OPTION, LOG_MAX_MB_OPTION,
		LOG_KEEP_OPTION, GPIO_OPTION, BUTTON_ACTIVE_HIGH_OPTION);
}

static bool has_any_source(const struct options *options)
{
	return options->read_stdin || options->replay_path != NULL || options->iio_path != NULL ||
	       options->iio_aux_path != NULL || options->gps_path != NULL;
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
		       struct device_status *status, struct source_stats *total)
{
	struct task tasks[MAX_TASKS];
	struct csv_player player;
	struct iio_accel primary_iio;
	struct iio_accel aux_iio;
	size_t count = 0;
	size_t i;
	int result = 0;
	bool player_open = false;
	bool primary_iio_open = false;
	bool aux_iio_open = false;
	int gps_fd = -1;
	int led_fd = -1;
	int button_fd = -1;

	memset(tasks, 0, sizeof(tasks));
	for (i = 0; i < MAX_TASKS; i++) {
		tasks[i].fd = -1;
		tasks[i].led_fd = -1;
		tasks[i].button_fd = -1;
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
	if (options->gpio_chip != NULL) {
		static const unsigned int leds[LED_COUNT] = {LED_RED_GPIO, LED_GREEN_GPIO,
							     LED_BLUE_GPIO};
		static const unsigned int buttons[BUTTON_COUNT] = {BUTTON1_GPIO, BUTTON2_GPIO};

		led_fd =
		    gpio_lines_request_outputs(options->gpio_chip, leds, LED_COUNT, GPIO_CONSUMER);
		if (led_fd < 0) {
			fprintf(stderr, "motion_logger: gpio %s (LEDs): %s\n", options->gpio_chip,
				strerror(-led_fd));
			result = led_fd;
			led_fd = -1;
			goto cleanup;
		}
		button_fd = gpio_lines_request_buttons(options->gpio_chip, buttons, BUTTON_COUNT,
						       !options->button_active_high,
						       BUTTON_DEBOUNCE_US, GPIO_CONSUMER);
		if (button_fd < 0) {
			fprintf(stderr, "motion_logger: gpio %s (buttons): %s\n",
				options->gpio_chip, strerror(-button_fd));
			result = button_fd;
			button_fd = -1;
			goto cleanup;
		}
		tasks[count].name = "gpio";
		tasks[count].run = run_gpio_task;
		tasks[count].status = status;
		tasks[count].led_fd = led_fd;
		tasks[count].button_fd = button_fd;
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

	/* Задача GPIO стоїть останньою: коли до неї дійшли, усі джерела даних уже завершились. */
	for (i = 0; i < count; i++) {
		if (!tasks[i].started) {
			continue;
		}
		if (tasks[i].run == run_gpio_task) {
			sources_finished = 1;
		}
		pthread_join(tasks[i].thread, NULL);
		total->received += tasks[i].stats.received;
		total->dropped += tasks[i].stats.dropped;
		total->errors += tasks[i].stats.errors;
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
	if (gps_fd >= 0) {
		close(gps_fd);
	}
	if (led_fd >= 0) {
		close(led_fd);
	}
	if (button_fd >= 0) {
		close(button_fd);
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
	task->stats.errors = stats.errors;

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

/*
 * Світлодіоди й кнопки: раз на LED_TICK_MS перераховує, що показувати (status_leds_compute), і
 * кладе в буфер запис на кожне натискання кнопки. При зупинці гасить усі світлодіоди.
 */
static int run_gpio_task(struct task *task)
{
	static const unsigned int buttons[BUTTON_COUNT] = {BUTTON1_GPIO, BUTTON2_GPIO};
	struct epoll_event event = {.events = EPOLLIN, .data.fd = task->button_fd};
	struct status_leds off = {false, false, false};
	struct status_leds shown = off;
	bool first = true;
	int epoll_fd = epoll_create1(EPOLL_CLOEXEC);
	int result = 0;

	if (epoll_fd < 0) {
		return -errno;
	}
	if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, task->button_fd, &event) < 0) {
		result = -errno;
		close(epoll_fd);
		return result;
	}

	while (!stop_requested && !sources_finished) {
		struct device_status_snapshot snapshot;
		struct status_leds leds;
		int ready = epoll_wait(epoll_fd, &event, 1, LED_TICK_MS);

		if (ready < 0 && errno != EINTR) {
			result = -errno;
			break;
		}
		if (ready > 0) {
			unsigned int offset;
			int64_t time_ms;
			int b;

			if (gpio_lines_read_event(task->button_fd, &offset, &time_ms) == 0) {
				for (b = 0; b < BUTTON_COUNT; b++) {
					struct sample item = {.source = SAMPLE_SOURCE_BUTTON,
							      .time_ms = time_ms,
							      .value = b + 1};

					if (buttons[b] != offset) {
						continue;
					}
					task->stats.received++;
					if (ring_buffer_push(task->buffer, &item) != 0) {
						task->stats.dropped++;
					}
				}
			}
		}

		snapshot = device_status_snapshot(task->status);
		leds = status_leds_compute(&snapshot, monotonic_ms());
		if (first || leds.red != shown.red || leds.green != shown.green ||
		    leds.blue != shown.blue) {
			show_leds(task->led_fd, &leds);
			shown = leds;
			first = false;
		}
	}

	show_leds(task->led_fd, &off);
	close(epoll_fd);

	return result;
}

static void show_leds(int led_fd, const struct status_leds *leds)
{
	uint64_t bits = (leds->red ? LED_RED_BIT : 0) | (leds->green ? LED_GREEN_BIT : 0) |
			(leds->blue ? LED_BLUE_BIT : 0);

	gpio_lines_set(led_fd, bits, LED_RED_BIT | LED_GREEN_BIT | LED_BLUE_BIT);
}
