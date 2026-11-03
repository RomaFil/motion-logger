/**
 * @file nmea_parser.c
 * @brief Реалізація розбору RMC і GGA. Опис полів і правил дивись у nmea_parser.h.
 *
 * Числа читаємо власними перевірками, а не голим strtod: strtod охоче приймає "inf",
 * "nan", пробіли й знак, а GPS-модуль їх надіслати не може, тож такі поля це помилка.
 * Десятковий роздільник у strtod залежить від локалі; програма локаль не змінює,
 * тож це завжди крапка.
 */
#include "nmea_parser.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "nmea.h"

#define MAX_FIELDS 20
#define SENTENCE_ID_LENGTH 5 /* talker (2 літери) + тип (3 літери), наприклад GPRMC */
#define TALKER_LENGTH 2

#define SECONDS_PER_MINUTE 60
#define SECONDS_PER_HOUR 3600
#define HOURS_PER_DAY 24
#define MINUTES_PER_HOUR 60
/* 60 секунд буває при високосній секунді. */
#define MAX_SECOND_VALUE 60

#define DEGREES_DIVISOR 100.0 /* у ddmm.mmmm дві цифри хвилин стоять праворуч від градусів */
#define MINUTES_PER_DEGREE 60.0
#define MAX_LATITUDE_DEGREES 90.0
#define MAX_LONGITUDE_DEGREES 180.0

/* У RMC рік двома цифрами: 80..99 це 1980-ті й 1990-ті (приклади зі стандарту), решта 2000-ні. */
#define YEAR_PIVOT 80
#define MONTHS_PER_YEAR 12
#define MAX_DAY_OF_MONTH 31
#define SECONDS_PER_DAY 86400

#define MAX_SATELLITE_DIGITS 2
#define MAX_QUALITY_DIGITS 1

/* Мінімум полів, без яких рядок не розібрати (включно з назвою рядка як нульовим). */
#define RMC_MIN_FIELDS 8
#define GGA_MIN_FIELDS 8

/* Номери полів у рядках (нульове це назва, наприклад GPRMC). */
enum rmc_field {
	RMC_TIME = 1,
	RMC_STATUS = 2,
	RMC_LATITUDE = 3,
	RMC_LATITUDE_HEMISPHERE = 4,
	RMC_LONGITUDE = 5,
	RMC_LONGITUDE_HEMISPHERE = 6,
	RMC_SPEED = 7,
	RMC_DATE = 9,
};

enum gga_field {
	GGA_TIME = 1,
	GGA_LATITUDE = 2,
	GGA_LATITUDE_HEMISPHERE = 3,
	GGA_LONGITUDE = 4,
	GGA_LONGITUDE_HEMISPHERE = 5,
	GGA_QUALITY = 6,
	GGA_SATELLITES = 7,
};

/* Тіло рядка, розрізане по комах. items[i] вказують усередину payload. */
struct sentence_fields {
	char payload[NMEA_MAX_SENTENCE_LENGTH + 1];
	char *items[MAX_FIELDS];
	size_t count;
};

/* Приватні функції описано тут, а визначено внизу: публічні функції йдуть першими. */
static int split_sentence(const char *sentence, const char *type, struct sentence_fields *fields);
static int parse_time(const char *field, struct nmea_fix *fix);
static int parse_date(const char *field, struct nmea_fix *fix);
static int64_t days_from_civil(int64_t year, unsigned int month, unsigned int day);
static int parse_position(const char *latitude, const char *latitude_hemisphere,
			  const char *longitude, const char *longitude_hemisphere,
			  struct nmea_fix *fix);
static int parse_coordinate(const char *value, const char *hemisphere, char positive, char negative,
			    double max_degrees, double *degrees);
static int parse_decimal(const char *text, double *number);
static int parse_small_integer(const char *text, size_t max_digits, unsigned int *number);
static bool is_digit(char c);
static unsigned int two_digits(const char *text);

int nmea_parse_rmc(const char *sentence, struct nmea_fix *fix)
{
	struct sentence_fields fields;
	struct nmea_fix result = {0};
	int error;

	if (fix == NULL) {
		return -EINVAL;
	}
	error = split_sentence(sentence, "RMC", &fields);
	if (error < 0) {
		return error;
	}
	if (fields.count < RMC_MIN_FIELDS) {
		return -EINVAL;
	}

	error = parse_time(fields.items[RMC_TIME], &result);
	if (error < 0) {
		return error;
	}

	/* Статус: A дійсні дані, V недійсні. Порожній статус вважаємо недійсним. */
	if (strcmp(fields.items[RMC_STATUS], "A") == 0) {
		result.valid = true;
	} else if (strcmp(fields.items[RMC_STATUS], "V") != 0 &&
		   fields.items[RMC_STATUS][0] != '\0') {
		return -EINVAL;
	}

	error = parse_position(fields.items[RMC_LATITUDE], fields.items[RMC_LATITUDE_HEMISPHERE],
			       fields.items[RMC_LONGITUDE], fields.items[RMC_LONGITUDE_HEMISPHERE],
			       &result);
	if (error < 0) {
		return error;
	}

	if (fields.items[RMC_SPEED][0] != '\0') {
		error = parse_decimal(fields.items[RMC_SPEED], &result.speed_knots);
		if (error < 0) {
			return error;
		}
		result.has_speed = true;
	}

	/* Дата необов'язкова: деякі модулі обрізають рядок після швидкості. */
	if (fields.count > RMC_DATE) {
		error = parse_date(fields.items[RMC_DATE], &result);
		if (error < 0) {
			return error;
		}
	}

	*fix = result;
	return 0;
}

int nmea_parse_gga(const char *sentence, struct nmea_fix *fix)
{
	struct sentence_fields fields;
	struct nmea_fix result = {0};
	unsigned int number;
	int error;

	if (fix == NULL) {
		return -EINVAL;
	}
	error = split_sentence(sentence, "GGA", &fields);
	if (error < 0) {
		return error;
	}
	if (fields.count < GGA_MIN_FIELDS) {
		return -EINVAL;
	}

	error = parse_time(fields.items[GGA_TIME], &result);
	if (error < 0) {
		return error;
	}

	error = parse_position(fields.items[GGA_LATITUDE], fields.items[GGA_LATITUDE_HEMISPHERE],
			       fields.items[GGA_LONGITUDE], fields.items[GGA_LONGITUDE_HEMISPHERE],
			       &result);
	if (error < 0) {
		return error;
	}

	/* Якість фіксації: 0 це "фіксації нема", більше нуля це є. Порожня вважається нулем. */
	if (fields.items[GGA_QUALITY][0] != '\0') {
		error = parse_small_integer(fields.items[GGA_QUALITY], MAX_QUALITY_DIGITS, &number);
		if (error < 0) {
			return error;
		}
		result.valid = number > 0;
	}

	if (fields.items[GGA_SATELLITES][0] != '\0') {
		error = parse_small_integer(fields.items[GGA_SATELLITES], MAX_SATELLITE_DIGITS,
					    &number);
		if (error < 0) {
			return error;
		}
		result.satellites = (uint8_t)number;
		result.has_satellites = true;
	}

	*fix = result;
	return 0;
}

/*
 * Перевіряє суму, перевіряє тип рядка (останні три літери назви), копіює тіло й розрізає
 * його по комах. Комою завершується кожне поле, тому порожні поля теж стають елементами.
 */
static int split_sentence(const char *sentence, const char *type, struct sentence_fields *fields)
{
	const char *star;
	size_t length;
	char *cursor;
	int error = nmea_verify_checksum(sentence);

	if (error < 0) {
		return error;
	}

	star = strchr(sentence, '*');
	length = (size_t)(star - sentence - 1);
	memcpy(fields->payload, sentence + 1, length);
	fields->payload[length] = '\0';

	fields->count = 0;
	cursor = fields->payload;
	for (;;) {
		char *comma;

		if (fields->count == MAX_FIELDS) {
			return -EINVAL;
		}
		fields->items[fields->count++] = cursor;

		comma = strchr(cursor, ',');
		if (comma == NULL) {
			break;
		}
		*comma = '\0';
		cursor = comma + 1;
	}

	/* Вид "GPRMC": talker (GP, GN, GL...) нас не цікавить, лише тип. */
	if (strlen(fields->items[0]) != SENTENCE_ID_LENGTH ||
	    strcmp(fields->items[0] + TALKER_LENGTH, type) != 0) {
		return -ENOMSG;
	}

	return 0;
}

/* Час "hhmmss" або "hhmmss.ss". Порожнє поле означає "часу ще нема". */
static int parse_time(const char *field, struct nmea_fix *fix)
{
	unsigned int hour;
	unsigned int minute;
	unsigned int second;
	size_t i;

	if (field[0] == '\0') {
		return 0;
	}

	for (i = 0; i < 6; i++) {
		if (!is_digit(field[i])) {
			return -EINVAL;
		}
	}
	if (field[6] != '\0') {
		/* Після шести цифр може йти лише частка секунди: крапка й принаймні одна цифра. */
		if (field[6] != '.' || !is_digit(field[7])) {
			return -EINVAL;
		}
		for (i = 7; field[i] != '\0'; i++) {
			if (!is_digit(field[i])) {
				return -EINVAL;
			}
		}
	}

	hour = two_digits(field);
	minute = two_digits(field + 2);
	second = two_digits(field + 4);
	if (hour >= HOURS_PER_DAY || minute >= MINUTES_PER_HOUR || second > MAX_SECOND_VALUE) {
		return -EINVAL;
	}

	fix->has_time = true;
	fix->seconds_of_day = hour * SECONDS_PER_HOUR + minute * SECONDS_PER_MINUTE + second;

	return 0;
}

/* Дата "ddmmyy". Порожнє поле означає "дати ще нема" (модуль не чув жодного супутника). */
static int parse_date(const char *field, struct nmea_fix *fix)
{
	unsigned int day;
	unsigned int month;
	unsigned int year;
	size_t i;

	if (field[0] == '\0') {
		return 0;
	}
	for (i = 0; i < 6; i++) {
		if (!is_digit(field[i])) {
			return -EINVAL;
		}
	}
	if (field[6] != '\0') {
		return -EINVAL;
	}

	day = two_digits(field);
	month = two_digits(field + 2);
	if (day < 1 || day > MAX_DAY_OF_MONTH || month < 1 || month > MONTHS_PER_YEAR) {
		return -EINVAL;
	}

	fix->has_date = true;
	fix->day = (uint8_t)day;
	fix->month = (uint8_t)month;
	year = two_digits(field + 4);
	fix->year = (uint16_t)(year >= YEAR_PIVOT ? 1900 + year : 2000 + year);

	return 0;
}

int64_t nmea_utc_seconds(const struct nmea_fix *fix)
{
	if (fix == NULL || !fix->has_date || !fix->has_time) {
		return -1;
	}

	return days_from_civil(fix->year, fix->month, fix->day) * SECONDS_PER_DAY +
	       fix->seconds_of_day;
}

/*
 * Скільки днів від 1970-01-01 до дати григоріанського календаря (алгоритм Говарда Хіннанта
 * "days_from_civil"): без таблиць місяців і без timegm(), якої нема в POSIX.
 */
static int64_t days_from_civil(int64_t year, unsigned int month, unsigned int day)
{
	int64_t era;
	unsigned int year_of_era;
	unsigned int day_of_year;
	unsigned int day_of_era;

	year -= month <= 2;
	era = year / 400;
	year_of_era = (unsigned int)(year - era * 400);
	day_of_year = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
	day_of_era = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;

	return era * 146097 + (int64_t)day_of_era - 719468;
}

/*
 * Широта і довгота разом. Усі чотири поля порожні це нормально ("позиції ще нема").
 * Якщо заповнено хоч одне, обидві координати мають бути цілими: parse_coordinate
 * відхиляє порожнє значення чи півкулю.
 */
static int parse_position(const char *latitude, const char *latitude_hemisphere,
			  const char *longitude, const char *longitude_hemisphere,
			  struct nmea_fix *fix)
{
	int error;

	if (latitude[0] == '\0' && latitude_hemisphere[0] == '\0' && longitude[0] == '\0' &&
	    longitude_hemisphere[0] == '\0') {
		return 0;
	}

	error = parse_coordinate(latitude, latitude_hemisphere, 'N', 'S', MAX_LATITUDE_DEGREES,
				 &fix->latitude_degrees);
	if (error < 0) {
		return error;
	}
	error = parse_coordinate(longitude, longitude_hemisphere, 'E', 'W', MAX_LONGITUDE_DEGREES,
				 &fix->longitude_degrees);
	if (error < 0) {
		return error;
	}

	fix->has_position = true;
	return 0;
}

/*
 * Одна координата. "ddmm.mmmm" (широта) чи "dddmm.mmmm" (довгота) перетворюється на
 * градуси: ціла частина від ділення на 100 це градуси, залишок це хвилини.
 */
static int parse_coordinate(const char *value, const char *hemisphere, char positive, char negative,
			    double max_degrees, double *degrees)
{
	double number;
	double whole_degrees;
	double minutes;
	int error;

	if (value[0] == '\0' || hemisphere[0] == '\0' || hemisphere[1] != '\0') {
		return -EINVAL;
	}
	if (hemisphere[0] != positive && hemisphere[0] != negative) {
		return -EINVAL;
	}

	error = parse_decimal(value, &number);
	if (error < 0) {
		return error;
	}

	whole_degrees = (double)(long)(number / DEGREES_DIVISOR);
	minutes = number - whole_degrees * DEGREES_DIVISOR;
	if (minutes >= MINUTES_PER_DEGREE) {
		return -EINVAL;
	}

	*degrees = whole_degrees + minutes / MINUTES_PER_DEGREE;
	if (*degrees > max_degrees) {
		return -EINVAL;
	}
	if (hemisphere[0] == negative) {
		*degrees = -*degrees;
	}

	return 0;
}

/* Додатне десяткове число "123", "123.45": цифри, не більше однієї крапки, цифра перед нею. */
static int parse_decimal(const char *text, double *number)
{
	size_t i = 0;

	if (!is_digit(text[0])) {
		return -EINVAL;
	}
	while (is_digit(text[i])) {
		i++;
	}
	if (text[i] == '.') {
		i++;
		if (!is_digit(text[i])) {
			return -EINVAL;
		}
		while (is_digit(text[i])) {
			i++;
		}
	}
	if (text[i] != '\0') {
		return -EINVAL;
	}

	*number = strtod(text, NULL);
	return 0;
}

/* Ціле число з 1..max_digits цифр, без знаку й пробілів. */
static int parse_small_integer(const char *text, size_t max_digits, unsigned int *number)
{
	size_t length = 0;
	unsigned int value = 0;

	while (is_digit(text[length])) {
		value = value * 10 + (unsigned int)(text[length] - '0');
		length++;
		if (length > max_digits) {
			return -EINVAL;
		}
	}
	if (length == 0 || text[length] != '\0') {
		return -EINVAL;
	}

	*number = value;
	return 0;
}

static bool is_digit(char c) { return c >= '0' && c <= '9'; }

/* Число з двох цифр на початку тексту; цифри вже перевірено. */
static unsigned int two_digits(const char *text)
{
	return (unsigned int)((text[0] - '0') * 10 + (text[1] - '0'));
}
