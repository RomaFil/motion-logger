/**
 * @file test_gps_reader.c
 * @brief Перевірки читача GPS на каналі (pipe): рядки NMEA подаються так, ніби їх шле модуль.
 *
 * Справжнього модуля не потрібно: читач працює з будь-яким дескриптором, тож тест пише рядки
 * в канал. Рядки з правильними контрольними сумами взято з tests/data/gps_sample.nmea.
 *
 * Запуск: make test (звичайний) або make test-tsan. Код виходу 0 означає, що все пройшло.
 */
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "check.h"
#include "gps_reader.h"
#include "ring_buffer.h"

#define EPSILON 1e-6
#define BUFFER_CAPACITY 64
#define OVERFLOW_LINE_LENGTH 300
#define FEED_CHUNK_BYTES 7
#define FEED_PAUSE_NS 200000L

#define GGA_FIX "$GPGGA,120008.00,5027.0060,N,03031.4040,E,1,07,1.2,180.0,M,28.0,M,,*67"
#define RMC_FIX "$GPRMC,120008.00,A,5027.0060,N,03031.4040,E,0.0,90.0,281026,,,A*64"
#define RMC_VOID_WITH_POSITION "$GPRMC,120008.00,V,5027.0060,N,03031.4040,E,0.0,90.0,281026,,,N*7C"
#define RMC_NO_FIX "$GPRMC,120000.00,V,,,,,,,281026,,,N*71"
#define RMC_BAD_SUM "$GPRMC,120008.00,A,5027.0060,N,03031.4040,E,0.0,90.0,281026,,,A*00"
#define GSV "$GPGSV,1,1,00*79"
#define TXT "$GPTXT,01,01,02,u-blox ag - www.u-blox.com*50"
#define NOT_NMEA "hello world"

/* Скільки перевірок не пройшло. main повертає це як код виходу. */
static int failures;

static volatile sig_atomic_t never_stop;

/* Канал і буфер, на яких ганяється читач. */
struct fixture {
	int pipe_fd[2];
	struct sample storage[BUFFER_CAPACITY];
	struct ring_buffer rb;
};

static void fixture_init(struct fixture *fx, size_t capacity)
{
	CHECK(pipe(fx->pipe_fd) == 0);
	CHECK(ring_buffer_init(&fx->rb, fx->storage, capacity) == 0);
	never_stop = 0;
}

static void fixture_destroy(struct fixture *fx)
{
	close(fx->pipe_fd[0]);
	ring_buffer_destroy(&fx->rb);
}

/* Пише текст у канал і закриває кінець запису: читач побачить кінець даних і вийде. */
static void feed_and_close(struct fixture *fx, const char *text)
{
	CHECK(write(fx->pipe_fd[1], text, strlen(text)) == (ssize_t)strlen(text));
	close(fx->pipe_fd[1]);
}

static int run_reader(struct fixture *fx, struct gps_reader_stats *stats)
{
	return gps_reader_run(fx->pipe_fd[0], &fx->rb, &never_stop, stats);
}

static void test_rejects_bad_arguments(void)
{
	struct fixture fx;
	struct gps_reader_stats stats;

	fixture_init(&fx, BUFFER_CAPACITY);
	CHECK(gps_reader_run(-1, &fx.rb, &never_stop, &stats) == -EINVAL);
	CHECK(gps_reader_run(fx.pipe_fd[0], NULL, &never_stop, &stats) == -EINVAL);
	CHECK(gps_reader_run(fx.pipe_fd[0], &fx.rb, NULL, &stats) == -EINVAL);
	CHECK(gps_reader_run(fx.pipe_fd[0], &fx.rb, &never_stop, NULL) == -EINVAL);
	close(fx.pipe_fd[1]);
	fixture_destroy(&fx);
}

/* Є фіксація: у записі позиція в десяткових градусах, швидкість і супутники з попереднього GGA. */
static void test_fix_sample_carries_position_and_satellites(void)
{
	struct fixture fx;
	struct gps_reader_stats stats;
	struct sample item;

	fixture_init(&fx, BUFFER_CAPACITY);
	feed_and_close(&fx, GGA_FIX "\n" RMC_FIX "\n");
	CHECK(run_reader(&fx, &stats) == 0);

	CHECK(stats.lines == 2);
	CHECK(stats.samples == 1);
	CHECK(stats.dropped == 0);
	CHECK(ring_buffer_pop(&fx.rb, &item) == 0);
	CHECK(item.source == SAMPLE_SOURCE_GPS);
	CHECK(item.gps_fix);
	CHECK(fabs(item.latitude_deg - 50.4501) < EPSILON);
	CHECK(fabs(item.longitude_deg - 30.5234) < EPSILON);
	CHECK(fabs(item.speed_knots) < EPSILON);
	CHECK(item.satellites == 7);
	CHECK(item.time_ms > 0);
	CHECK(ring_buffer_is_empty(&fx.rb));

	fixture_destroy(&fx);
}

/* Нема фіксації: запис усе одно з'являється, але без координат і без вигаданих нулів. */
static void test_no_fix_sample_has_no_coordinates(void)
{
	struct fixture fx;
	struct gps_reader_stats stats;
	struct sample item;

	fixture_init(&fx, BUFFER_CAPACITY);
	feed_and_close(&fx, RMC_NO_FIX "\n");
	CHECK(run_reader(&fx, &stats) == 0);

	CHECK(stats.samples == 1);
	CHECK(ring_buffer_pop(&fx.rb, &item) == 0);
	CHECK(item.source == SAMPLE_SOURCE_GPS);
	CHECK(!item.gps_fix);
	CHECK(fabs(item.latitude_deg) < EPSILON);
	CHECK(fabs(item.longitude_deg) < EPSILON);
	CHECK(item.satellites == -1);

	fixture_destroy(&fx);
}

/*
 * Модуль без фіксації може повторювати останню відому позицію зі статусом V. Дійсність дає
 * статус, а не наявність цифр у полях: така позиція в лог не йде.
 */
static void test_void_status_with_position_is_not_a_fix(void)
{
	struct fixture fx;
	struct gps_reader_stats stats;
	struct sample item;

	fixture_init(&fx, BUFFER_CAPACITY);
	feed_and_close(&fx, RMC_VOID_WITH_POSITION "\n");
	CHECK(run_reader(&fx, &stats) == 0);

	CHECK(stats.samples == 1);
	CHECK(stats.bad_checksum == 0);
	CHECK(ring_buffer_pop(&fx.rb, &item) == 0);
	CHECK(!item.gps_fix);
	CHECK(fabs(item.latitude_deg) < EPSILON);
	CHECK(fabs(item.longitude_deg) < EPSILON);

	fixture_destroy(&fx);
}

/* GGA лише запам'ятовується: окремого запису від нього нема. Інші типи рядків пропускаються. */
static void test_only_rmc_makes_samples(void)
{
	struct fixture fx;
	struct gps_reader_stats stats;

	fixture_init(&fx, BUFFER_CAPACITY);
	feed_and_close(&fx, TXT "\n" GGA_FIX "\n" GSV "\n");
	CHECK(run_reader(&fx, &stats) == 0);

	CHECK(stats.lines == 3);
	CHECK(stats.samples == 0);
	CHECK(stats.bad_checksum == 0);
	CHECK(stats.malformed == 0);
	CHECK(ring_buffer_is_empty(&fx.rb));

	fixture_destroy(&fx);
}

/* Рядок із помилкою рахується, а наступний правильний усе одно приймається. */
static void test_bad_lines_are_counted_and_skipped(void)
{
	struct fixture fx;
	struct gps_reader_stats stats;

	fixture_init(&fx, BUFFER_CAPACITY);
	feed_and_close(&fx, RMC_BAD_SUM "\n" NOT_NMEA "\n"
					"$GPRMC,1*\n" RMC_FIX "\n");
	CHECK(run_reader(&fx, &stats) == 0);

	CHECK(stats.lines == 4);
	CHECK(stats.bad_checksum == 1);
	CHECK(stats.malformed == 2);
	CHECK(stats.samples == 1);

	fixture_destroy(&fx);
}

/* Закінчення рядка \r\n (так шле модуль) і порожні рядки не заважають. */
static void test_crlf_and_blank_lines(void)
{
	struct fixture fx;
	struct gps_reader_stats stats;

	fixture_init(&fx, BUFFER_CAPACITY);
	feed_and_close(&fx, "\r\n" RMC_FIX "\r\n\n" RMC_NO_FIX "\r\n");
	CHECK(run_reader(&fx, &stats) == 0);

	CHECK(stats.lines == 2);
	CHECK(stats.samples == 2);
	CHECK(stats.malformed == 0);

	fixture_destroy(&fx);
}

/* Задовгий рядок викидається цілком і рахується; наступний рядок читається нормально. */
static void test_overlong_line_is_dropped_whole(void)
{
	struct fixture fx;
	struct gps_reader_stats stats;
	char text[OVERFLOW_LINE_LENGTH + sizeof(RMC_FIX) + 8];
	size_t length;

	memset(text, 'A', OVERFLOW_LINE_LENGTH);
	length = OVERFLOW_LINE_LENGTH;
	length += (size_t)snprintf(text + length, sizeof(text) - length, "\n%s\n", RMC_FIX);

	fixture_init(&fx, BUFFER_CAPACITY);
	CHECK(write(fx.pipe_fd[1], text, length) == (ssize_t)length);
	close(fx.pipe_fd[1]);
	CHECK(run_reader(&fx, &stats) == 0);

	CHECK(stats.lines == 2);
	CHECK(stats.malformed == 1);
	CHECK(stats.samples == 1);

	fixture_destroy(&fx);
}

/* Повний буфер: записи не губляться мовчки, а рахуються в dropped. */
static void test_full_buffer_counts_dropped(void)
{
	struct fixture fx;
	struct gps_reader_stats stats;
	const size_t capacity = 2;
	const size_t total = 5;
	size_t i;

	fixture_init(&fx, capacity);
	for (i = 0; i < total; i++) {
		CHECK(write(fx.pipe_fd[1], RMC_NO_FIX "\n", sizeof(RMC_NO_FIX)) ==
		      (ssize_t)sizeof(RMC_NO_FIX));
	}
	close(fx.pipe_fd[1]);
	CHECK(run_reader(&fx, &stats) == 0);

	CHECK(stats.samples == total);
	CHECK(stats.dropped == total - capacity);
	CHECK(ring_buffer_count(&fx.rb) == capacity);

	fixture_destroy(&fx);
}

/* Прапорець зупинки вже піднято: читач виходить одразу, не чекаючи даних із порожнього каналу. */
static void test_stop_flag_ends_the_run(void)
{
	struct fixture fx;
	struct gps_reader_stats stats;
	volatile sig_atomic_t stop = 1;

	fixture_init(&fx, BUFFER_CAPACITY);
	CHECK(gps_reader_run(fx.pipe_fd[0], &fx.rb, &stop, &stats) == 0);
	CHECK(stats.lines == 0);
	close(fx.pipe_fd[1]);
	fixture_destroy(&fx);
}

/* Подавач пише дрібними шматками посеред рядків, як це робить послідовний порт. */
struct feeder {
	int fd;
	const char *text;
};

static void *feed_in_small_chunks(void *arg)
{
	struct feeder *feeder = arg;
	size_t length = strlen(feeder->text);
	size_t offset = 0;
	const struct timespec pause = {.tv_sec = 0, .tv_nsec = FEED_PAUSE_NS};

	while (offset < length) {
		size_t chunk =
		    length - offset < FEED_CHUNK_BYTES ? length - offset : FEED_CHUNK_BYTES;

		if (write(feeder->fd, feeder->text + offset, chunk) != (ssize_t)chunk) {
			break;
		}
		offset += chunk;
		nanosleep(&pause, NULL);
	}
	close(feeder->fd);

	return NULL;
}

static void test_line_split_across_reads(void)
{
	struct fixture fx;
	struct gps_reader_stats stats;
	struct feeder feeder;
	pthread_t thread;
	struct sample item;

	fixture_init(&fx, BUFFER_CAPACITY);
	feeder.fd = fx.pipe_fd[1];
	feeder.text = GGA_FIX "\n" RMC_FIX "\n";
	CHECK(pthread_create(&thread, NULL, feed_in_small_chunks, &feeder) == 0);

	CHECK(run_reader(&fx, &stats) == 0);
	CHECK(pthread_join(thread, NULL) == 0);

	CHECK(stats.lines == 2);
	CHECK(stats.samples == 1);
	CHECK(ring_buffer_pop(&fx.rb, &item) == 0);
	CHECK(item.gps_fix);
	CHECK(item.satellites == 7);

	fixture_destroy(&fx);
}

int main(void)
{
	test_rejects_bad_arguments();
	test_fix_sample_carries_position_and_satellites();
	test_no_fix_sample_has_no_coordinates();
	test_void_status_with_position_is_not_a_fix();
	test_only_rmc_makes_samples();
	test_bad_lines_are_counted_and_skipped();
	test_crlf_and_blank_lines();
	test_overlong_line_is_dropped_whole();
	test_full_buffer_counts_dropped();
	test_stop_flag_ends_the_run();
	test_line_split_across_reads();

	if (failures != 0) {
		printf("%d check(s) failed\n", failures);
		return 1;
	}
	printf("gps_reader: all checks passed\n");

	return 0;
}
