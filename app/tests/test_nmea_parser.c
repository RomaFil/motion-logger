/**
 * @file test_nmea_parser.c
 * @brief Перевірки розбору рядків RMC і GGA: значення, порожні поля, півкулі, помилки.
 *
 * Запуск: make test (звичайний) або make test-tsan. Код виходу 0 означає, що всі
 * перевірки пройшли. Заліза не потрібно.
 */
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "check.h"
#include "nmea.h"
#include "nmea_parser.h"

/* Відомі приклади з опису NMEA (суми 47 і 6A перевірено ще в кроці 3.2). */
#define GGA "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47"
#define RMC "$GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W*6A"

/* Допустима різниця при порівнянні дробових чисел: далеко менша за метр на карті. */
#define TOLERANCE 0.000001

/* 12:35:19 = 12 * 3600 + 35 * 60 + 19 */
#define TIME_123519 45319

/* Скільки перевірок не пройшло. main повертає це як код виходу. */
static int failures;

static bool nearly_equal(double a, double b) { return a - b < TOLERANCE && b - a < TOLERANCE; }

/*
 * Складає з тіла повний рядок "$тіло*СУМА": суму рахує наш же код, тож тест не залежить
 * від вручну переписаних шістнадцяткових цифр.
 */
static const char *make_sentence(const char *body)
{
	static char sentence[NMEA_MAX_SENTENCE_LENGTH + 1];

	snprintf(sentence, sizeof(sentence), "$%s*%02X", body,
		 nmea_compute_checksum(body, strlen(body)));

	return sentence;
}

static void test_rmc_example_is_parsed(void)
{
	struct nmea_fix fix;

	CHECK(nmea_parse_rmc(RMC, &fix) == 0);
	CHECK(fix.has_time && fix.seconds_of_day == TIME_123519);
	CHECK(fix.valid);
	CHECK(fix.has_position);
	CHECK(nearly_equal(fix.latitude_degrees, 48.0 + 7.038 / 60.0));
	CHECK(nearly_equal(fix.longitude_degrees, 11.0 + 31.0 / 60.0));
	CHECK(fix.has_speed && nearly_equal(fix.speed_knots, 22.4));
	CHECK(!fix.has_satellites);
}

static void test_gga_example_is_parsed(void)
{
	struct nmea_fix fix;

	CHECK(nmea_parse_gga(GGA, &fix) == 0);
	CHECK(fix.has_time && fix.seconds_of_day == TIME_123519);
	CHECK(fix.valid);
	CHECK(fix.has_position);
	CHECK(nearly_equal(fix.latitude_degrees, 48.0 + 7.038 / 60.0));
	CHECK(nearly_equal(fix.longitude_degrees, 11.0 + 31.0 / 60.0));
	CHECK(fix.has_satellites && fix.satellites == 8);
	CHECK(!fix.has_speed);
}

/* Модуль щойно ввімкнено: супутників нема, майже всі поля порожні. Це не помилка. */
static void test_rmc_without_fix_has_empty_fields(void)
{
	struct nmea_fix fix;

	CHECK(nmea_parse_rmc(make_sentence("GPRMC,235947.00,V,,,,,,,061026,,,N"), &fix) == 0);
	CHECK(fix.has_time && fix.seconds_of_day == 23 * 3600 + 59 * 60 + 47);
	CHECK(!fix.valid);
	CHECK(!fix.has_position);
	CHECK(!fix.has_speed);
}

static void test_gga_without_fix_has_empty_fields(void)
{
	struct nmea_fix fix;

	CHECK(nmea_parse_gga(make_sentence("GPGGA,235947.00,,,,,0,00,99.99,,,,,,"), &fix) == 0);
	CHECK(fix.has_time);
	CHECK(!fix.valid);
	CHECK(!fix.has_position);
	/* "00" це не порожнє поле, а справжній нуль супутників. */
	CHECK(fix.has_satellites && fix.satellites == 0);
}

static void test_completely_empty_time_and_fields_are_accepted(void)
{
	struct nmea_fix fix;

	CHECK(nmea_parse_gga(make_sentence("GPGGA,,,,,,,,,,,,,,"), &fix) == 0);
	CHECK(!fix.has_time && !fix.valid && !fix.has_position && !fix.has_satellites);
}

static void test_south_and_west_are_negative(void)
{
	struct nmea_fix fix;

	CHECK(
	    nmea_parse_rmc(make_sentence("GPRMC,000000,A,3345.0000,S,15112.5000,W,0.0,,010125,,,A"),
			   &fix) == 0);
	CHECK(nearly_equal(fix.latitude_degrees, -33.75));
	CHECK(nearly_equal(fix.longitude_degrees, -(151.0 + 12.5 / 60.0)));
	CHECK(fix.has_speed && nearly_equal(fix.speed_knots, 0.0));
	CHECK(fix.seconds_of_day == 0);
}

static void test_other_talker_ids_are_accepted(void)
{
	struct nmea_fix fix;

	CHECK(nmea_parse_rmc(make_sentence("GNRMC,123519,A,4807.038,N,01131.000,E,1.0,,230394,,"),
			     &fix) == 0);
	CHECK(nmea_parse_gga(make_sentence("GLGGA,123519,4807.038,N,01131.000,E,2,10,0.9,5,M,,M,,"),
			     &fix) == 0);
	CHECK(fix.satellites == 10);
}

static void test_last_valid_moments_are_accepted(void)
{
	struct nmea_fix fix;

	CHECK(nmea_parse_rmc(make_sentence("GPRMC,235959,A,,,,,,,230394,,"), &fix) == 0);
	CHECK(fix.seconds_of_day == 24 * 3600 - 1);
	/* 23:59:60 буває в день високосної секунди. */
	CHECK(nmea_parse_rmc(make_sentence("GPRMC,235960,A,,,,,,,230394,,"), &fix) == 0);
}

static void test_fractional_seconds_are_dropped(void)
{
	struct nmea_fix fix;

	CHECK(nmea_parse_rmc(make_sentence("GPRMC,123519.75,A,,,,,,,230394,,"), &fix) == 0);
	CHECK(fix.seconds_of_day == TIME_123519);
}

static void test_wrong_sentence_type_is_reported(void)
{
	struct nmea_fix fix;

	CHECK(nmea_parse_rmc(GGA, &fix) == -ENOMSG);
	CHECK(nmea_parse_gga(RMC, &fix) == -ENOMSG);
	CHECK(nmea_parse_rmc(make_sentence("GPGLL,4916.45,N,12311.12,W,225444,A,"), &fix) ==
	      -ENOMSG);
}

static void test_bad_checksum_and_missing_checksum_are_reported(void)
{
	struct nmea_fix fix;

	CHECK(nmea_parse_gga("$GPGGA,123519,5807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47",
			     &fix) == -EBADMSG);
	CHECK(nmea_parse_rmc("$GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W",
			     &fix) == -EINVAL);
}

static void test_invalid_fields_are_rejected(void)
{
	struct nmea_fix fix;

	/* Час */
	CHECK(nmea_parse_rmc(make_sentence("GPRMC,240000,A,,,,,,,230394,,"), &fix) == -EINVAL);
	CHECK(nmea_parse_rmc(make_sentence("GPRMC,236000,A,,,,,,,230394,,"), &fix) == -EINVAL);
	CHECK(nmea_parse_rmc(make_sentence("GPRMC,235961,A,,,,,,,230394,,"), &fix) == -EINVAL);
	CHECK(nmea_parse_rmc(make_sentence("GPRMC,256100,A,,,,,,,230394,,"), &fix) == -EINVAL);
	CHECK(nmea_parse_rmc(make_sentence("GPRMC,126100,A,,,,,,,230394,,"), &fix) == -EINVAL);
	CHECK(nmea_parse_rmc(make_sentence("GPRMC,12351,A,,,,,,,230394,,"), &fix) == -EINVAL);
	CHECK(nmea_parse_rmc(make_sentence("GPRMC,123519.,A,,,,,,,230394,,"), &fix) == -EINVAL);
	CHECK(nmea_parse_rmc(make_sentence("GPRMC,12a519,A,,,,,,,230394,,"), &fix) == -EINVAL);
	/* Статус */
	CHECK(nmea_parse_rmc(make_sentence("GPRMC,123519,Z,,,,,,,230394,,"), &fix) == -EINVAL);
	/* Широта і довгота */
	CHECK(nmea_parse_rmc(make_sentence("GPRMC,123519,A,4860.000,N,01131.000,E,,,,,"), &fix) ==
	      -EINVAL);
	CHECK(nmea_parse_rmc(make_sentence("GPRMC,123519,A,9100.000,N,01131.000,E,,,,,"), &fix) ==
	      -EINVAL);
	CHECK(nmea_parse_rmc(make_sentence("GPRMC,123519,A,4807.038,N,18100.000,E,,,,,"), &fix) ==
	      -EINVAL);
	CHECK(nmea_parse_rmc(make_sentence("GPRMC,123519,A,4807.038,X,01131.000,E,,,,,"), &fix) ==
	      -EINVAL);
	CHECK(nmea_parse_rmc(make_sentence("GPRMC,123519,A,4807.038,E,01131.000,E,,,,,"), &fix) ==
	      -EINVAL);
	CHECK(nmea_parse_rmc(make_sentence("GPRMC,123519,A,4807.038,,01131.000,E,,,,,"), &fix) ==
	      -EINVAL);
	CHECK(nmea_parse_rmc(make_sentence("GPRMC,123519,A,4807.038,N,,,,,,,"), &fix) == -EINVAL);
	CHECK(nmea_parse_rmc(make_sentence("GPRMC,123519,A,48x7.038,N,01131.000,E,,,,,"), &fix) ==
	      -EINVAL);
	/* Швидкість */
	CHECK(nmea_parse_rmc(make_sentence("GPRMC,123519,A,,,,,abc,,230394,,"), &fix) == -EINVAL);
	CHECK(nmea_parse_rmc(make_sentence("GPRMC,123519,A,,,,,-1.0,,230394,,"), &fix) == -EINVAL);
	CHECK(nmea_parse_rmc(make_sentence("GPRMC,123519,A,,,,,inf,,230394,,"), &fix) == -EINVAL);
	/* Якість і супутники */
	CHECK(nmea_parse_gga(make_sentence("GPGGA,123519,,,,,x,08,0.9,,,,,,"), &fix) == -EINVAL);
	CHECK(nmea_parse_gga(make_sentence("GPGGA,123519,,,,,1,ab,0.9,,,,,,"), &fix) == -EINVAL);
	CHECK(nmea_parse_gga(make_sentence("GPGGA,123519,,,,,1,100,0.9,,,,,,"), &fix) == -EINVAL);
}

static void test_too_few_fields_are_rejected(void)
{
	struct nmea_fix fix;

	CHECK(nmea_parse_rmc(make_sentence("GPRMC,123519,A"), &fix) == -EINVAL);
	CHECK(nmea_parse_gga(make_sentence("GPGGA,123519,4807.038,N"), &fix) == -EINVAL);
}

static void test_failed_parse_leaves_result_untouched(void)
{
	struct nmea_fix fix;
	struct nmea_fix before;

	memset(&fix, 0x5A, sizeof(fix));
	before = fix;

	CHECK(nmea_parse_rmc(make_sentence("GPRMC,256100,A,,,,,,,230394,,"), &fix) == -EINVAL);
	CHECK(memcmp(&fix, &before, sizeof(fix)) == 0);
	CHECK(nmea_parse_gga(RMC, &fix) == -ENOMSG);
	CHECK(memcmp(&fix, &before, sizeof(fix)) == 0);
}

static void test_null_arguments_are_rejected(void)
{
	struct nmea_fix fix;

	CHECK(nmea_parse_rmc(NULL, &fix) == -EINVAL);
	CHECK(nmea_parse_rmc(RMC, NULL) == -EINVAL);
	CHECK(nmea_parse_gga(NULL, &fix) == -EINVAL);
	CHECK(nmea_parse_gga(GGA, NULL) == -EINVAL);
}

/* Курс і дата з RMC: 084,4° і 28.10.2026; двозначний рік це 2000 + yy. */
static void test_rmc_course_and_date_are_parsed(void)
{
	struct nmea_fix fix;

	CHECK(nmea_parse_rmc(
		  make_sentence("GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,281026,003.1,W"),
		  &fix) == 0);
	CHECK(fix.has_course && nearly_equal(fix.course_degrees, 84.4));
	CHECK(fix.has_date && fix.year == 2026 && fix.month == 10 && fix.day == 28);
}

/* Без фіксації курсу й дати може не бути: це нормально, прапорці лишаються false. */
static void test_rmc_without_course_and_date(void)
{
	struct nmea_fix fix;

	CHECK(nmea_parse_rmc(make_sentence("GPRMC,235947.00,V,,,,,,,,,,N"), &fix) == 0);
	CHECK(!fix.has_course);
	CHECK(!fix.has_date);

	/* Короткий рядок без полів курсу й дати теж приймається. */
	CHECK(nmea_parse_rmc(make_sentence("GPRMC,123519,A,4807.038,N,01131.000,E,022.4"), &fix) ==
	      0);
	CHECK(!fix.has_course && !fix.has_date);
}

static void test_rmc_bad_course_or_date_is_rejected(void)
{
	struct nmea_fix fix;

	CHECK(nmea_parse_rmc(
		  make_sentence("GPRMC,123519,A,4807.038,N,01131.000,E,022.4,361.0,281026,,"),
		  &fix) == -EINVAL);
	CHECK(nmea_parse_rmc(
		  make_sentence("GPRMC,123519,A,4807.038,N,01131.000,E,022.4,84.4,321026,,"),
		  &fix) == -EINVAL);
	CHECK(nmea_parse_rmc(
		  make_sentence("GPRMC,123519,A,4807.038,N,01131.000,E,022.4,84.4,281326,,"),
		  &fix) == -EINVAL);
	CHECK(nmea_parse_rmc(
		  make_sentence("GPRMC,123519,A,4807.038,N,01131.000,E,022.4,84.4,28102,,"),
		  &fix) == -EINVAL);
	CHECK(nmea_parse_rmc(
		  make_sentence("GPRMC,123519,A,4807.038,N,01131.000,E,022.4,84.4,2810x6,,"),
		  &fix) == -EINVAL);
}

/* HDOP і висота з GGA; висота може бути від'ємною (нижче рівня моря). */
static void test_gga_hdop_and_altitude(void)
{
	struct nmea_fix fix;

	CHECK(nmea_parse_gga(GGA, &fix) == 0);
	CHECK(fix.has_hdop && nearly_equal(fix.hdop, 0.9));
	CHECK(fix.has_altitude && nearly_equal(fix.altitude_m, 545.4));

	CHECK(nmea_parse_gga(
		  make_sentence("GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,-12.5,M,46.9,M,,"),
		  &fix) == 0);
	CHECK(fix.has_altitude && nearly_equal(fix.altitude_m, -12.5));
}

static void test_gga_without_hdop_and_altitude(void)
{
	struct nmea_fix fix;

	CHECK(nmea_parse_gga(make_sentence("GPGGA,235947.00,,,,,0,00,,,,,,,"), &fix) == 0);
	CHECK(!fix.has_hdop);
	CHECK(!fix.has_altitude);
}

static void test_gga_bad_hdop_or_altitude_is_rejected(void)
{
	struct nmea_fix fix;

	CHECK(nmea_parse_gga(
		  make_sentence("GPGGA,123519,4807.038,N,01131.000,E,1,08,abc,545.4,M,46.9,M,,"),
		  &fix) == -EINVAL);
	CHECK(nmea_parse_gga(
		  make_sentence("GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,5.4.5,M,46.9,M,,"),
		  &fix) == -EINVAL);
	CHECK(nmea_parse_gga(
		  make_sentence("GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,--5,M,46.9,M,,"),
		  &fix) == -EINVAL);
}

int main(void)
{
	test_rmc_course_and_date_are_parsed();
	test_rmc_without_course_and_date();
	test_rmc_bad_course_or_date_is_rejected();
	test_gga_hdop_and_altitude();
	test_gga_without_hdop_and_altitude();
	test_gga_bad_hdop_or_altitude_is_rejected();
	test_rmc_example_is_parsed();
	test_gga_example_is_parsed();
	test_rmc_without_fix_has_empty_fields();
	test_gga_without_fix_has_empty_fields();
	test_completely_empty_time_and_fields_are_accepted();
	test_south_and_west_are_negative();
	test_other_talker_ids_are_accepted();
	test_last_valid_moments_are_accepted();
	test_fractional_seconds_are_dropped();
	test_wrong_sentence_type_is_reported();
	test_bad_checksum_and_missing_checksum_are_reported();
	test_invalid_fields_are_rejected();
	test_too_few_fields_are_rejected();
	test_failed_parse_leaves_result_untouched();
	test_null_arguments_are_rejected();

	if (failures != 0) {
		printf("%d check(s) failed\n", failures);
		return 1;
	}
	printf("nmea_parser: all checks passed\n");

	return 0;
}
