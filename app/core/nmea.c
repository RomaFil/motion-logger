/**
 * @file nmea.c
 * @brief Реалізація перевірки рядків NMEA. Опис формату й правил дивись у nmea.h.
 */
#include "nmea.h"

#include <errno.h>
#include <stdbool.h>
#include <string.h>

#define HEX_BASE 16
#define HEX_LETTER_OFFSET 10

/* Приватні функції описано тут, а визначено внизу: публічні функції йдуть першими. */
static int hex_digit_value(char c);
static bool is_valid_line_end(const char *tail);

uint8_t nmea_compute_checksum(const char *payload, size_t length)
{
	uint8_t checksum = 0;
	size_t i;

	for (i = 0; i < length; i++) {
		checksum ^= (uint8_t)payload[i];
	}

	return checksum;
}

int nmea_verify_checksum(const char *sentence)
{
	const char *star;
	int high_digit;
	int low_digit;
	uint8_t expected;

	if (sentence == NULL || sentence[0] != '$') {
		return -EINVAL;
	}
	if (strnlen(sentence, NMEA_MAX_SENTENCE_LENGTH + 1) > NMEA_MAX_SENTENCE_LENGTH) {
		return -EINVAL;
	}

	star = strchr(sentence, '*');
	if (star == NULL) {
		return -EINVAL;
	}

	/* Друга цифра читається лише після першої: так ми не вийдемо за кінець рядка. */
	high_digit = hex_digit_value(star[1]);
	if (high_digit < 0) {
		return -EINVAL;
	}
	low_digit = hex_digit_value(star[2]);
	if (low_digit < 0 || !is_valid_line_end(star + 3)) {
		return -EINVAL;
	}

	/* '$' усередині тіла означає, що два рядки склеїлись: це не один цілий рядок. */
	if (memchr(sentence + 1, '$', (size_t)(star - sentence - 1)) != NULL) {
		return -EINVAL;
	}

	expected = (uint8_t)(high_digit * HEX_BASE + low_digit);
	if (nmea_compute_checksum(sentence + 1, (size_t)(star - sentence - 1)) != expected) {
		return -EBADMSG;
	}

	return 0;
}

/* Значення шістнадцяткової цифри (0-9, A-F, a-f), або -1, якщо символ не цифра. */
static int hex_digit_value(char c)
{
	if (c >= '0' && c <= '9') {
		return c - '0';
	}
	if (c >= 'A' && c <= 'F') {
		return c - 'A' + HEX_LETTER_OFFSET;
	}
	if (c >= 'a' && c <= 'f') {
		return c - 'a' + HEX_LETTER_OFFSET;
	}

	return -1;
}

/* Після контрольної суми має бути кінець рядка: нічого, "\n" або "\r\n". */
static bool is_valid_line_end(const char *tail)
{
	return tail[0] == '\0' || strcmp(tail, "\n") == 0 || strcmp(tail, "\r\n") == 0;
}
