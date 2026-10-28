/**
 * @file test_nmea.c
 * @brief Перевірки контрольної суми NMEA: правильні рядки, пошкоджені, неправильної форми.
 *
 * Запуск: make test (звичайний) або make test-tsan. Код виходу 0 означає, що всі
 * перевірки пройшли. Заліза не потрібно.
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "check.h"
#include "nmea.h"

/*
 * Три правильні рядки з опису протоколу NMEA (суми 47, 6A, 1D) перераховано вручну
 * XOR-ом і збігаються з відомими прикладами.
 */
#define GGA "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47"
#define RMC "$GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W*6A"
#define GLL "$GPGLL,4916.45,N,12311.12,W,225444,A,*1D"

/* Той самий GGA, але в одній цифрі широти замість 4 стоїть 5: сума вже не підходить. */
#define GGA_CORRUPTED "$GPGGA,123519,5807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47"

/* Скільки перевірок не пройшло. main повертає це як код виходу. */
static int failures;

static void test_three_valid_sentences_are_accepted(void)
{
	CHECK(nmea_verify_checksum(GGA) == 0);
	CHECK(nmea_verify_checksum(RMC) == 0);
	CHECK(nmea_verify_checksum(GLL) == 0);
}

static void test_corrupted_sentence_is_rejected(void)
{
	CHECK(nmea_verify_checksum(GGA_CORRUPTED) == -EBADMSG);
}

static void test_wrong_checksum_digits_are_rejected(void)
{
	CHECK(nmea_verify_checksum("$GPGLL,4916.45,N,12311.12,W,225444,A,*1E") == -EBADMSG);
	CHECK(nmea_verify_checksum("$GPGLL,4916.45,N,12311.12,W,225444,A,*00") == -EBADMSG);
}

static void test_line_endings_and_letter_case_are_accepted(void)
{
	CHECK(nmea_verify_checksum(GLL "\n") == 0);
	CHECK(nmea_verify_checksum(GLL "\r\n") == 0);
	CHECK(nmea_verify_checksum(RMC "\r\n") == 0);
	/* Суму 6A можна записати і малими літерами. */
	CHECK(nmea_verify_checksum(
		  "$GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W*6a") == 0);
}

static void test_empty_payload_has_zero_checksum(void)
{
	CHECK(nmea_compute_checksum("", 0) == 0);
	CHECK(nmea_verify_checksum("$*00") == 0);
	CHECK(nmea_verify_checksum("$*01") == -EBADMSG);
}

static void test_compute_checksum_matches_known_value(void)
{
	const char payload[] = "GPGLL,4916.45,N,12311.12,W,225444,A,";

	CHECK(nmea_compute_checksum(payload, strlen(payload)) == 0x1D);
	/* Тільки перші два символи "GP": 'G' ^ 'P' = 0x47 ^ 0x50 = 0x17. */
	CHECK(nmea_compute_checksum(payload, 2) == 0x17);
}

static void test_malformed_sentences_are_invalid(void)
{
	CHECK(nmea_verify_checksum(NULL) == -EINVAL);
	CHECK(nmea_verify_checksum("") == -EINVAL);
	CHECK(nmea_verify_checksum("GPGLL,4916.45,N,12311.12,W,225444,A,*1D") == -EINVAL);
	CHECK(nmea_verify_checksum("$GPGLL,4916.45,N,12311.12,W,225444,A,") == -EINVAL);
	CHECK(nmea_verify_checksum("$GPGLL,4916.45,N,12311.12,W,225444,A,*") == -EINVAL);
	CHECK(nmea_verify_checksum("$GPGLL,4916.45,N,12311.12,W,225444,A,*1") == -EINVAL);
	CHECK(nmea_verify_checksum("$GPGLL,4916.45,N,12311.12,W,225444,A,*1G") == -EINVAL);
	CHECK(nmea_verify_checksum("$GPGLL,4916.45,N,12311.12,W,225444,A,*1D1") == -EINVAL);
	CHECK(nmea_verify_checksum(GLL " junk") == -EINVAL);
	CHECK(nmea_verify_checksum(GLL "\r") == -EINVAL);
}

static void test_glued_sentences_are_invalid(void)
{
	/* Початок одного рядка і цілий інший: у тілі з'явився другий '$'. */
	CHECK(nmea_verify_checksum("$GPGLL,4916.45$GPGLL,4916.45,N,12311.12,W,225444,A,*1D") ==
	      -EINVAL);
}

static void test_too_long_sentence_is_invalid(void)
{
	char sentence[NMEA_MAX_SENTENCE_LENGTH + 20];
	const char tail[] = "*00";
	size_t body_length = NMEA_MAX_SENTENCE_LENGTH + 5;

	sentence[0] = '$';
	memset(sentence + 1, 'A', body_length);
	strcpy(sentence + 1 + body_length, tail);

	CHECK(nmea_verify_checksum(sentence) == -EINVAL);
}

/* Рядок, зібраний нашим кодом із власної суми, завжди проходить перевірку. */
static void test_generated_sentences_round_trip(void)
{
	const char *bodies[] = {"GPGGA,,,,,,0,00,99.99,,,,,,", "GNRMC,001043.00,V,,,,,,,,,,N", "X"};
	size_t i;

	for (i = 0; i < sizeof(bodies) / sizeof(bodies[0]); i++) {
		char sentence[NMEA_MAX_SENTENCE_LENGTH + 1];
		uint8_t checksum = nmea_compute_checksum(bodies[i], strlen(bodies[i]));

		snprintf(sentence, sizeof(sentence), "$%s*%02X\r\n", bodies[i], checksum);
		CHECK(nmea_verify_checksum(sentence) == 0);
	}
}

int main(void)
{
	test_three_valid_sentences_are_accepted();
	test_corrupted_sentence_is_rejected();
	test_wrong_checksum_digits_are_rejected();
	test_line_endings_and_letter_case_are_accepted();
	test_empty_payload_has_zero_checksum();
	test_compute_checksum_matches_known_value();
	test_malformed_sentences_are_invalid();
	test_glued_sentences_are_invalid();
	test_too_long_sentence_is_invalid();
	test_generated_sentences_round_trip();

	if (failures != 0) {
		printf("%d check(s) failed\n", failures);
		return 1;
	}
	printf("nmea: all checks passed\n");

	return 0;
}
