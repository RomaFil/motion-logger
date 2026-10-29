/**
 * @file gps_reader.c
 * @brief Реалізація читача GPS. Опис і правила дивись у gps_reader.h.
 */
#include "gps_reader.h"

#include <errno.h>
#include <stdbool.h>
#include <string.h>
#include <sys/epoll.h>
#include <unistd.h>

#include "io_util.h"
#include "nmea_parser.h"
#include "utc_time.h"

/* Як часто прокидаємось перевірити прапорець зупинки, якщо даних нема. */
#define POLL_TIMEOUT_MS 100

#define READ_CHUNK_BYTES 256
#define SATELLITES_UNKNOWN (-1)

/* Збирач рядка: байти додаються по одному, на '\n' рядок готовий. */
struct line_assembler {
	char text[GPS_READER_LINE_MAX];
	size_t length;
	bool overflow; /* рядок не влізає: решту до '\n' викидаємо */
};

/* Усе, що читач пам'ятає між викликами read(). */
struct reader_state {
	struct line_assembler line;
	int satellites;	     /* з останнього GGA, або SATELLITES_UNKNOWN */
	struct nmea_fix gga; /* останній розібраний GGA: висота й HDOP */
	bool has_gga;
};

/* Приватні функції описано тут, а визначено внизу: публічна функція йде першою. */
static int register_source(int epoll_fd, int source_fd);
static int read_available(int source_fd, struct ring_buffer *rb, struct reader_state *state,
			  struct gps_reader_stats *stats);
static void feed_byte(char byte, struct ring_buffer *rb, struct reader_state *state,
		      struct gps_reader_stats *stats);
static void handle_line(const char *line, struct ring_buffer *rb, struct reader_state *state,
			struct gps_reader_stats *stats);
static void store_gps_sample(const struct nmea_fix *fix, const struct reader_state *state,
			     struct ring_buffer *rb, struct gps_reader_stats *stats);

int gps_reader_run(int source_fd, struct ring_buffer *rb, const volatile sig_atomic_t *stop,
		   struct gps_reader_stats *stats)
{
	struct reader_state state = {.satellites = SATELLITES_UNKNOWN};
	struct epoll_event event;
	int epoll_fd;
	int result = 0;

	if (source_fd < 0 || rb == NULL || stop == NULL || stats == NULL) {
		return -EINVAL;
	}
	memset(stats, 0, sizeof(*stats));

	epoll_fd = epoll_create1(EPOLL_CLOEXEC);
	if (epoll_fd < 0) {
		return -errno;
	}

	result = register_source(epoll_fd, source_fd);
	if (result < 0) {
		goto cleanup;
	}

	while (!*stop) {
		int ready = epoll_wait(epoll_fd, &event, 1, POLL_TIMEOUT_MS);

		if (ready < 0) {
			if (errno == EINTR) {
				continue;
			}
			result = -errno;
			goto cleanup;
		}
		if (ready == 0) {
			continue;
		}

		result = read_available(source_fd, rb, &state, stats);
		if (result != 0) {
			result = result < 0 ? result : 0; /* додатне значення: джерело закрите */
			goto cleanup;
		}
	}

cleanup:
	close(epoll_fd);
	return result;
}

static int register_source(int epoll_fd, int source_fd)
{
	struct epoll_event event = {.events = EPOLLIN, .data.fd = source_fd};

	if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, source_fd, &event) < 0) {
		return -errno;
	}

	return 0;
}

/* Один read(): 0 = читати далі, 1 = джерело закрите, мінус errno = помилка. */
static int read_available(int source_fd, struct ring_buffer *rb, struct reader_state *state,
			  struct gps_reader_stats *stats)
{
	char chunk[READ_CHUNK_BYTES];
	ssize_t bytes_read = read(source_fd, chunk, sizeof(chunk));
	ssize_t i;

	if (bytes_read < 0) {
		if (errno == EINTR || errno == EAGAIN) {
			return 0;
		}
		return -errno;
	}
	if (bytes_read == 0) {
		return 1;
	}

	for (i = 0; i < bytes_read; i++) {
		feed_byte(chunk[i], rb, state, stats);
	}

	return 0;
}

static void feed_byte(char byte, struct ring_buffer *rb, struct reader_state *state,
		      struct gps_reader_stats *stats)
{
	struct line_assembler *line = &state->line;

	if (byte == '\r') {
		return;
	}
	if (byte != '\n') {
		if (line->length + 1 < sizeof(line->text)) {
			line->text[line->length++] = byte;
		} else {
			line->overflow = true;
		}
		return;
	}

	if (line->overflow) {
		stats->lines++;
		stats->malformed++;
	} else if (line->length > 0) {
		line->text[line->length] = '\0';
		handle_line(line->text, rb, state, stats);
	}
	line->length = 0;
	line->overflow = false;
}

/* Розбирає один готовий рядок: RMC дає запис у буфер, GGA лише запам'ятовує супутники. */
static void handle_line(const char *line, struct ring_buffer *rb, struct reader_state *state,
			struct gps_reader_stats *stats)
{
	struct nmea_fix fix;
	int result;

	stats->lines++;

	result = nmea_parse_rmc(line, &fix);
	if (result == 0) {
		store_gps_sample(&fix, state, rb, stats);
		return;
	}
	if (result == -ENOMSG) {
		result = nmea_parse_gga(line, &fix);
		if (result == 0) {
			state->satellites =
			    fix.has_satellites ? (int)fix.satellites : SATELLITES_UNKNOWN;
			state->gga = fix;
			state->has_gga = true;
			return;
		}
		if (result == -ENOMSG) {
			return; /* інший тип рядка (GSV, TXT…): нам не потрібен */
		}
	}

	if (result == -EBADMSG) {
		stats->bad_checksum++;
	} else {
		stats->malformed++;
	}
}

static void store_gps_sample(const struct nmea_fix *fix, const struct reader_state *state,
			     struct ring_buffer *rb, struct gps_reader_stats *stats)
{
	struct sample item = {.source = SAMPLE_SOURCE_GPS,
			      .time_ms = monotonic_ms(),
			      .gps_fix = fix->valid && fix->has_position,
			      .satellites = state->satellites};

	if (item.gps_fix) {
		item.latitude_deg = fix->latitude_degrees;
		item.longitude_deg = fix->longitude_degrees;
		item.speed_knots = fix->has_speed ? fix->speed_knots : 0.0;
		/* Час і дату довіряємо лише з дійсним статусом A: до фіксації модуль може віддавати
		 * час зі своєї пам’яті, а не з супутників. */
		if (fix->has_time && fix->has_date) {
			item.has_utc = true;
			item.utc_epoch_s =
			    utc_time_to_epoch(fix->year, fix->month, fix->day, fix->seconds_of_day);
		}
		item.has_course = fix->has_course;
		item.course_deg = fix->course_degrees;
		if (state->has_gga) {
			item.has_altitude = state->gga.has_altitude;
			item.altitude_m = state->gga.altitude_m;
			item.has_hdop = state->gga.has_hdop;
			item.hdop = state->gga.hdop;
		}
	}

	stats->samples++;
	if (ring_buffer_push(rb, &item) != 0) {
		stats->dropped++;
	}
}
