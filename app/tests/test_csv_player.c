/**
 * @file test_csv_player.c
 * @brief Перевірки драйвера-програвача csv_player: розбір файлу, пропуск коментарів, помилки,
 *        швидкість програвання.
 *
 * Запуск: make test (звичайний) або make test-tsan. Тимчасові файли тест створює в /tmp
 * і видаляє сам.
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "check.h"
#include "csv_player.h"
#include "io_util.h"

#define TEMP_PATH_SIZE 32
#define PACED_DURATION_MS 150
#define PACED_SLACK_MS 20

/* Скільки перевірок не пройшло. main повертає це як код виходу. */
static int failures;

/* Шаблон імені тимчасового файлу для mkstemp. */
static const char TEMP_TEMPLATE[] = "/tmp/test_csv_XXXXXX";

/* Створює тимчасовий файл із заданим вмістом і кладе його шлях у path. */
static void make_csv(char *path, const char *content)
{
	int fd;

	strcpy(path, TEMP_TEMPLATE);
	fd = mkstemp(path);
	CHECK(fd >= 0);
	if (fd >= 0) {
		CHECK(write(fd, content, strlen(content)) == (ssize_t)strlen(content));
		close(fd);
	}
}

static void test_open_rejects_bad_arguments(void)
{
	struct csv_player player;

	CHECK(csv_player_open(NULL, "/tmp/x", 0) == -EINVAL);
	CHECK(csv_player_open(&player, NULL, 0) == -EINVAL);
	CHECK(csv_player_open(&player, "/tmp/x", -1.0) == -EINVAL);
	CHECK(csv_player_open(&player, "/tmp/x", 0.0 / 0.0) == -EINVAL);
}

static void test_open_reports_missing_file(void)
{
	struct csv_player player;

	CHECK(csv_player_open(&player, "/no_such_dir/accel.csv", 0) == -ENOENT);
}

/* Відліки віддаються з усіма полями в порядку файлу, потім -ENODATA; коментарі пропущено. */
static void test_samples_come_in_file_order_and_comments_are_skipped(void)
{
	char path[TEMP_PATH_SIZE];
	struct csv_player player;
	struct accel_sample sample;

	make_csv(path, "# SYNTHETIC\n\n0,0.01,-0.02,1.00\n10,0.5,0.25,-1.5\r\n");
	CHECK(csv_player_open(&player, path, 0) == 0);

	CHECK(player.base.read(&player.base, &sample) == 0);
	CHECK(sample.time_ms == 0);
	CHECK(sample.x_g == 0.01 && sample.y_g == -0.02 && sample.z_g == 1.0);

	CHECK(player.base.read(&player.base, &sample) == 0);
	CHECK(sample.time_ms == 10);
	CHECK(sample.x_g == 0.5 && sample.y_g == 0.25 && sample.z_g == -1.5);

	CHECK(player.base.read(&player.base, &sample) == -ENODATA);
	CHECK(player.base.read(&player.base, &sample) == -ENODATA); /* і далі те саме */

	player.base.close(&player.base);
	unlink(path);
}

/* Зіпсований рядок це -EINVAL, а номер рядка видно для повідомлення про помилку. */
static void test_bad_line_reports_error_and_line_number(void)
{
	char path[TEMP_PATH_SIZE];
	struct csv_player player;
	struct accel_sample sample;

	make_csv(path, "0,0,0,1\n# comment\n10,abc,0,1\n");
	CHECK(csv_player_open(&player, path, 0) == 0);

	CHECK(player.base.read(&player.base, &sample) == 0);
	CHECK(player.base.read(&player.base, &sample) == -EINVAL);
	CHECK(player.line_number == 3);

	player.base.close(&player.base);
	unlink(path);
}

static void test_empty_file_means_no_data(void)
{
	char path[TEMP_PATH_SIZE];
	struct csv_player player;
	struct accel_sample sample;

	make_csv(path, "");
	CHECK(csv_player_open(&player, path, 0) == 0);
	CHECK(player.base.read(&player.base, &sample) == -ENODATA);

	player.base.close(&player.base);
	unlink(path);
}

/* Скільки мілісекунд іде програвання файлу з відліками 0, 75, 150 мс на заданій швидкості. */
static int64_t measure_playback_ms(double speed)
{
	char path[TEMP_PATH_SIZE];
	struct csv_player player;
	struct accel_sample sample;
	int64_t started_ms;
	int64_t elapsed_ms;

	make_csv(path, "0,0,0,1\n75,0,0,1\n150,0,0,1\n");
	CHECK(csv_player_open(&player, path, speed) == 0);

	started_ms = monotonic_ms();
	while (player.base.read(&player.base, &sample) == 0) {
	}
	elapsed_ms = monotonic_ms() - started_ms;

	player.base.close(&player.base);
	unlink(path);

	return elapsed_ms;
}

/* speed 1: 150 мс запису займають приблизно 150 мс; не швидше (і не в рази повільніше). */
static void test_realtime_speed_waits_for_each_sample(void)
{
	int64_t elapsed_ms = measure_playback_ms(1.0);

	CHECK(elapsed_ms >= PACED_DURATION_MS - 2);
	CHECK(elapsed_ms < PACED_DURATION_MS + PACED_SLACK_MS * 10);
}

/* speed 10: у десять разів швидше. */
static void test_faster_speed_shortens_playback(void)
{
	int64_t elapsed_ms = measure_playback_ms(10.0);

	CHECK(elapsed_ms >= PACED_DURATION_MS / 10 - 2);
	CHECK(elapsed_ms < PACED_DURATION_MS / 2);
}

/* speed 0: без пауз. */
static void test_zero_speed_does_not_wait(void)
{
	CHECK(measure_playback_ms(0.0) < PACED_SLACK_MS);
}

int main(void)
{
	test_open_rejects_bad_arguments();
	test_open_reports_missing_file();
	test_samples_come_in_file_order_and_comments_are_skipped();
	test_bad_line_reports_error_and_line_number();
	test_empty_file_means_no_data();
	test_realtime_speed_waits_for_each_sample();
	test_faster_speed_shortens_playback();
	test_zero_speed_does_not_wait();

	if (failures != 0) {
		printf("%d check(s) failed\n", failures);
		return 1;
	}
	printf("csv_player: all checks passed\n");

	return 0;
}
