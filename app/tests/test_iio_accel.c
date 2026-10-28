/**
 * @file test_iio_accel.c
 * @brief Перевірки драйвера iio_accel на «підробному» каталозі пристрою IIO (звичайні файли в
 * /tmp).
 *
 * Справжнього датчика не потрібно: драйвер читає лише файли in_accel_*_raw та *_scale, тому
 * тест створює їх сам і змінює між читаннями, як це робило б ядро.
 *
 * Запуск: make test (звичайний) або make test-tsan. Код виходу 0 означає, що все пройшло.
 */
#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "check.h"
#include "iio_accel.h"
#include "io_util.h"

#define EPSILON 1e-9
#define STANDARD_GRAVITY_MS2 "9.80665"
#define RATE_HZ 100
#define PERIOD_MS 10
#define SAMPLES_FOR_TIMING 5

/* Скільки перевірок не пройшло. main повертає це як код виходу. */
static int failures;

static const char TEMP_TEMPLATE[] = "/tmp/test_iio_XXXXXX";

/* Створює (або переписує) файл name у каталозі dir із текстом content. */
static void write_file(const char *dir, const char *name, const char *content)
{
	char path[IIO_ACCEL_PATH_SIZE];
	FILE *file;

	snprintf(path, sizeof(path), "%s/%s", dir, name);
	file = fopen(path, "w");
	CHECK(file != NULL);
	if (file != NULL) {
		fputs(content, file);
		fclose(file);
	}
}

static void remove_file(const char *dir, const char *name)
{
	char path[IIO_ACCEL_PATH_SIZE];

	snprintf(path, sizeof(path), "%s/%s", dir, name);
	unlink(path);
}

/* Каталог пристрою зі спільним in_accel_scale і осями raw = x, y, z. */
static void make_device(char *dir, const char *x, const char *y, const char *z)
{
	strcpy(dir, TEMP_TEMPLATE);
	CHECK(mkdtemp(dir) != NULL);
	write_file(dir, "in_accel_x_raw", x);
	write_file(dir, "in_accel_y_raw", y);
	write_file(dir, "in_accel_z_raw", z);
	write_file(dir, "in_accel_scale", STANDARD_GRAVITY_MS2 "\n");
}

static void remove_device(const char *dir)
{
	static const char *const names[] = {
	    "in_accel_x_raw",	"in_accel_y_raw",   "in_accel_z_raw",  "in_accel_scale",
	    "in_accel_x_scale", "in_accel_y_scale", "in_accel_z_scale"};
	size_t i;

	for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		remove_file(dir, names[i]);
	}
	rmdir(dir);
}

static void test_open_rejects_bad_arguments(void)
{
	struct iio_accel dev;
	char long_path[IIO_ACCEL_PATH_SIZE + 16];

	memset(long_path, 'a', sizeof(long_path) - 1);
	long_path[sizeof(long_path) - 1] = '\0';

	CHECK(iio_accel_open(NULL, "/tmp", RATE_HZ) == -EINVAL);
	CHECK(iio_accel_open(&dev, NULL, RATE_HZ) == -EINVAL);
	CHECK(iio_accel_open(&dev, "/tmp", 0) == -EINVAL);
	CHECK(iio_accel_open(&dev, "/tmp", 1001) == -EINVAL);
	CHECK(iio_accel_open(&dev, long_path, RATE_HZ) == -EINVAL);
}

static void test_open_reports_missing_device(void)
{
	struct iio_accel dev;

	CHECK(iio_accel_open(&dev, "/no_such_iio_device", RATE_HZ) == -ENOENT);
}

/* Сирі відліки перетворюються в g за масштабом: 9,80665 м/с² на одиницю дає рівно 1 g на одиницю.
 */
static void test_samples_are_converted_to_g(void)
{
	char dir[sizeof(TEMP_TEMPLATE)];
	struct iio_accel dev;
	struct accel_sample sample;

	make_device(dir, "1\n", "-2\n", "3\n");
	CHECK(iio_accel_open(&dev, dir, RATE_HZ) == 0);

	CHECK(dev.base.read(&dev.base, &sample) == 0);
	CHECK(fabs(sample.x_g - 1.0) < EPSILON);
	CHECK(fabs(sample.y_g + 2.0) < EPSILON);
	CHECK(fabs(sample.z_g - 3.0) < EPSILON);

	dev.base.close(&dev.base);
	remove_device(dir);
}

/* Реальний масштаб MPU6050 ±2 g: 16384 одиниці = 1 g, тобто scale = 9,80665 / 16384 м/с². */
static void test_real_mpu6050_scale(void)
{
	char dir[sizeof(TEMP_TEMPLATE)];
	struct iio_accel dev;
	struct accel_sample sample;

	make_device(dir, "0\n", "0\n", "16384\n");
	write_file(dir, "in_accel_scale", "0.000598550415\n");
	CHECK(iio_accel_open(&dev, dir, RATE_HZ) == 0);

	CHECK(dev.base.read(&dev.base, &sample) == 0);
	CHECK(fabs(sample.z_g - 1.0) < 1e-6);

	dev.base.close(&dev.base);
	remove_device(dir);
}

/* Нема спільного масштабу: беремо in_accel_<вісь>_scale для кожної осі. */
static void test_per_axis_scale_fallback(void)
{
	char dir[sizeof(TEMP_TEMPLATE)];
	struct iio_accel dev;
	struct accel_sample sample;

	make_device(dir, "1\n", "1\n", "1\n");
	remove_file(dir, "in_accel_scale");
	write_file(dir, "in_accel_x_scale", STANDARD_GRAVITY_MS2 "\n");
	write_file(dir, "in_accel_y_scale", "19.6133\n");
	write_file(dir, "in_accel_z_scale", "4.903325\n");
	CHECK(iio_accel_open(&dev, dir, RATE_HZ) == 0);

	CHECK(dev.base.read(&dev.base, &sample) == 0);
	CHECK(fabs(sample.x_g - 1.0) < EPSILON);
	CHECK(fabs(sample.y_g - 2.0) < EPSILON);
	CHECK(fabs(sample.z_g - 0.5) < EPSILON);

	dev.base.close(&dev.base);
	remove_device(dir);
}

static void test_open_rejects_bad_scale(void)
{
	char dir[sizeof(TEMP_TEMPLATE)];
	struct iio_accel dev;
	static const char *const bad[] = {"0\n", "-9.8\n", "nan\n", "inf\n", "abc\n"};
	size_t i;

	make_device(dir, "0\n", "0\n", "0\n");
	for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		int result;

		write_file(dir, "in_accel_scale", bad[i]);
		result = iio_accel_open(&dev, dir, RATE_HZ);
		CHECK(result == -EPROTO || result == -EIO);
	}
	remove_device(dir);
}

/* Драйвер читає файл заново при кожному відліку: показання ядра змінюються між викликами. */
static void test_new_value_is_read_on_every_call(void)
{
	char dir[sizeof(TEMP_TEMPLATE)];
	struct iio_accel dev;
	struct accel_sample sample;

	make_device(dir, "1\n", "0\n", "0\n");
	CHECK(iio_accel_open(&dev, dir, RATE_HZ) == 0);

	CHECK(dev.base.read(&dev.base, &sample) == 0);
	CHECK(fabs(sample.x_g - 1.0) < EPSILON);

	write_file(dir, "in_accel_x_raw", "5\n");
	CHECK(dev.base.read(&dev.base, &sample) == 0);
	CHECK(fabs(sample.x_g - 5.0) < EPSILON);

	dev.base.close(&dev.base);
	remove_device(dir);
}

/* Нечислове значення в raw це -EIO, а не мовчазний нуль. */
static void test_garbage_in_raw_is_an_error(void)
{
	char dir[sizeof(TEMP_TEMPLATE)];
	struct iio_accel dev;
	struct accel_sample sample;

	make_device(dir, "1\n", "0\n", "0\n");
	CHECK(iio_accel_open(&dev, dir, RATE_HZ) == 0);

	write_file(dir, "in_accel_y_raw", "oops\n");
	CHECK(dev.base.read(&dev.base, &sample) == -EIO);

	dev.base.close(&dev.base);
	remove_device(dir);
}

/* Мітки часу ростуть, а N відліків займають приблизно (N-1) періодів: драйвер сам тримає частоту.
 */
static void test_samples_follow_the_requested_rate(void)
{
	char dir[sizeof(TEMP_TEMPLATE)];
	struct iio_accel dev;
	struct accel_sample sample;
	int64_t first_ms = 0;
	int64_t previous_ms = -1;
	int i;

	make_device(dir, "0\n", "0\n", "1\n");
	CHECK(iio_accel_open(&dev, dir, RATE_HZ) == 0);

	for (i = 0; i < SAMPLES_FOR_TIMING; i++) {
		CHECK(dev.base.read(&dev.base, &sample) == 0);
		if (i == 0) {
			first_ms = sample.time_ms;
		}
		CHECK(sample.time_ms > previous_ms);
		previous_ms = sample.time_ms;
	}
	CHECK(previous_ms - first_ms >= (SAMPLES_FOR_TIMING - 1) * PERIOD_MS - 2);
	CHECK(previous_ms - first_ms < (SAMPLES_FOR_TIMING - 1) * PERIOD_MS + 100);

	dev.base.close(&dev.base);
	remove_device(dir);
}

int main(void)
{
	test_open_rejects_bad_arguments();
	test_open_reports_missing_device();
	test_samples_are_converted_to_g();
	test_real_mpu6050_scale();
	test_per_axis_scale_fallback();
	test_open_rejects_bad_scale();
	test_new_value_is_read_on_every_call();
	test_garbage_in_raw_is_an_error();
	test_samples_follow_the_requested_rate();

	if (failures != 0) {
		printf("%d check(s) failed\n", failures);
		return 1;
	}
	printf("iio_accel: all checks passed\n");

	return 0;
}
