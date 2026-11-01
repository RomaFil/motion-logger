/**
 * @file analyze_recording.c
 * @brief Прогоняє запис акселерометра (CSV) і GPS (NMEA) через модулі проєкту й друкує звіт.
 *
 * Використання:
 *   analyze_recording [-w вікно] [-t поріг_g] [-c підтвердження] accel.csv [gps.nmea]
 *
 *   -w  розмір вікна фільтра руху, відліків (за замовчуванням 50)
 *   -t  поріг руху в g (за замовчуванням 0.02, підібрано за записом 01.11.2026)
 *   -c  скільки відповідей фільтра підряд підтверджують зміну стану (за замовчуванням 50)
 *
 * Звіт: таблиця "секунда, рух так/ні, середнє відхилення в g", список подій MOVE START /
 * MOVE STOP з часом, підсумок по акселерометру і (якщо задано файл) по GPS-рядках.
 * Інструмент слугує для кроку 3.5: підібрати поріг і вікно так, щоб таблиця збігалась з тим,
 * що відбувалось насправді (див. tests/data/ground_truth.txt).
 */
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "accel_csv.h"
#include "motion_defaults.h"
#include "motion_report.h"
#include "nmea.h"
#include "nmea_parser.h"

#define DEFAULT_WINDOW MOTION_DEFAULT_WINDOW
#define DEFAULT_THRESHOLD_G MOTION_DEFAULT_THRESHOLD_G
#define DEFAULT_CONFIRM MOTION_DEFAULT_CONFIRM
#define NMEA_LINE_SIZE 128
#define MSEC_PER_SEC 1000
#define MAX_EVENTS 256

struct settings {
	size_t window;
	double threshold_g;
	size_t confirm;
	const char *accel_path;
	const char *gps_path; /* NULL, якщо GPS-файл не задано */
};

struct recorded_event {
	enum motion_event event;
	int64_t time_ms;
};

struct accel_totals {
	size_t samples;
	size_t rejected_lines;
};

/* Приватні функції описано тут, а визначено внизу: main іде першою. */
static int parse_arguments(int argc, char **argv, struct settings *settings);
static int parse_size_option(const char *text, size_t *value);
static int parse_double_option(const char *text, double *value);
static int analyze_accel(FILE *file, const struct settings *settings);
static void print_row(const struct motion_report_row *row);
static void print_event(enum motion_event event, int64_t time_ms);
static int analyze_gps(FILE *file);
static void print_usage(const char *program);

int main(int argc, char **argv)
{
	struct settings settings;
	FILE *file;
	int result;

	if (parse_arguments(argc, argv, &settings) < 0) {
		print_usage(argv[0]);
		return EXIT_FAILURE;
	}

	file = fopen(settings.accel_path, "r");
	if (file == NULL) {
		fprintf(stderr, "%s: %s\n", settings.accel_path, strerror(errno));
		return EXIT_FAILURE;
	}
	result = analyze_accel(file, &settings);
	fclose(file);
	if (result < 0) {
		fprintf(stderr, "analyze_recording: %s\n", strerror(-result));
		return EXIT_FAILURE;
	}

	if (settings.gps_path != NULL) {
		file = fopen(settings.gps_path, "r");
		if (file == NULL) {
			fprintf(stderr, "%s: %s\n", settings.gps_path, strerror(errno));
			return EXIT_FAILURE;
		}
		result = analyze_gps(file);
		fclose(file);
		if (result < 0) {
			fprintf(stderr, "analyze_recording: %s\n", strerror(-result));
			return EXIT_FAILURE;
		}
	}

	return EXIT_SUCCESS;
}

static int parse_arguments(int argc, char **argv, struct settings *settings)
{
	int option;

	settings->window = DEFAULT_WINDOW;
	settings->threshold_g = DEFAULT_THRESHOLD_G;
	settings->confirm = DEFAULT_CONFIRM;
	settings->accel_path = NULL;
	settings->gps_path = NULL;

	while ((option = getopt(argc, argv, "w:t:c:")) != -1) {
		int error = -EINVAL;

		switch (option) {
		case 'w':
			error = parse_size_option(optarg, &settings->window);
			break;
		case 't':
			error = parse_double_option(optarg, &settings->threshold_g);
			break;
		case 'c':
			error = parse_size_option(optarg, &settings->confirm);
			break;
		default:
			break;
		}
		if (error < 0) {
			return -EINVAL;
		}
	}

	if (optind >= argc || argc - optind > 2) {
		return -EINVAL;
	}
	settings->accel_path = argv[optind];
	if (argc - optind == 2) {
		settings->gps_path = argv[optind + 1];
	}

	return 0;
}

/* Додатне ціле з командного рядка; усе інше це помилка. */
static int parse_size_option(const char *text, size_t *value)
{
	char *end;
	long parsed;

	errno = 0;
	parsed = strtol(text, &end, 10);
	if (errno != 0 || end == text || *end != '\0' || parsed <= 0) {
		return -EINVAL;
	}

	*value = (size_t)parsed;
	return 0;
}

static int parse_double_option(const char *text, double *value)
{
	char *end;
	double parsed;

	errno = 0;
	parsed = strtod(text, &end);
	if (errno != 0 || end == text || *end != '\0' || !(parsed >= 0.0)) {
		return -EINVAL;
	}

	*value = parsed;
	return 0;
}

static int analyze_accel(FILE *file, const struct settings *settings)
{
	struct motion_report report;
	struct motion_report_update update;
	struct accel_totals totals = {0};
	struct accel_sample sample;
	char line[ACCEL_CSV_MAX_LINE];
	int error;

	error =
	    motion_report_init(&report, settings->window, settings->threshold_g, settings->confirm);
	if (error < 0) {
		return error;
	}

	printf("# window=%zu threshold_g=%.3f confirm=%zu\n", settings->window,
	       settings->threshold_g, settings->confirm);
	printf("second moving samples avg_dev_g\n");

	/* Події не друкуємо посеред таблиці: збираємо й виводимо після неї. */
	struct recorded_event events[MAX_EVENTS];
	size_t event_count = 0;
	size_t events_dropped = 0;

	while (fgets(line, sizeof(line), file) != NULL) {
		error = accel_csv_parse_line(line, &sample);
		if (error == -ENODATA) {
			continue;
		}
		if (error < 0) {
			totals.rejected_lines++;
			continue;
		}

		error = motion_report_add_sample(&report, &sample, &update);
		if (error < 0) {
			totals.rejected_lines++;
			continue;
		}
		totals.samples++;

		if (update.row_ready) {
			print_row(&update.row);
		}
		if (update.event == MOTION_EVENT_NONE) {
			continue;
		}
		if (event_count == MAX_EVENTS) {
			events_dropped++;
			continue;
		}
		events[event_count].event = update.event;
		events[event_count].time_ms = sample.time_ms;
		event_count++;
	}

	error = motion_report_finish(&report, &update);
	if (error < 0) {
		return error;
	}
	if (update.row_ready) {
		print_row(&update.row);
	}

	printf("events:\n");
	for (size_t i = 0; i < event_count; i++) {
		print_event(events[i].event, events[i].time_ms);
	}
	if (events_dropped > 0) {
		printf("  (ще %zu подій не показано: надто часті зміни, збільш -c)\n",
		       events_dropped);
	}
	printf("accel: %zu samples, %zu rejected lines\n", totals.samples, totals.rejected_lines);

	return 0;
}

static void print_row(const struct motion_report_row *row)
{
	printf("%6lld %-6s %7zu %.4f\n", (long long)row->second, row->moving ? "yes" : "no",
	       row->samples, row->average_deviation_g);
}

static void print_event(enum motion_event event, int64_t time_ms)
{
	printf("  %lld.%03lld s  %s\n", (long long)(time_ms / MSEC_PER_SEC),
	       (long long)(time_ms % MSEC_PER_SEC),
	       event == MOTION_EVENT_START ? "MOVE START" : "MOVE STOP");
}

/* Підсумок по рядках NMEA: скільки яких, скільки пошкоджено, перша й остання дійсна позиція. */
static int analyze_gps(FILE *file)
{
	char line[NMEA_LINE_SIZE];
	struct nmea_fix fix;
	size_t lines = 0;
	size_t gga = 0;
	size_t rmc = 0;
	size_t other = 0;
	size_t bad_checksum = 0;
	size_t malformed = 0;
	size_t valid_fixes = 0;
	size_t no_fix = 0;
	bool has_position = false;
	double latitude = 0.0;
	double longitude = 0.0;

	while (fgets(line, sizeof(line), file) != NULL) {
		int checksum;
		int parsed;

		lines++;
		checksum = nmea_verify_checksum(line);
		if (checksum == -EBADMSG) {
			bad_checksum++;
			continue;
		}
		if (checksum < 0) {
			malformed++;
			continue;
		}

		parsed = nmea_parse_rmc(line, &fix);
		if (parsed == 0) {
			rmc++;
		} else if (parsed == -ENOMSG) {
			parsed = nmea_parse_gga(line, &fix);
			if (parsed == 0) {
				gga++;
			}
		}
		if (parsed == -ENOMSG) {
			other++;
			continue;
		}
		if (parsed < 0) {
			malformed++;
			continue;
		}

		if (fix.valid && fix.has_position) {
			valid_fixes++;
			has_position = true;
			latitude = fix.latitude_degrees;
			longitude = fix.longitude_degrees;
		} else {
			no_fix++;
		}
	}

	printf("gps: %zu lines: %zu RMC, %zu GGA, %zu other, %zu bad checksum, %zu malformed\n",
	       lines, rmc, gga, other, bad_checksum, malformed);
	printf("gps: %zu fixes with position, %zu without\n", valid_fixes, no_fix);
	if (has_position) {
		printf("gps: last position %.5f, %.5f\n", latitude, longitude);
	}

	return 0;
}

static void print_usage(const char *program)
{
	fprintf(stderr,
		"usage: %s [-w window] [-t threshold_g] [-c confirm] accel.csv [gps.nmea]\n",
		program);
}
