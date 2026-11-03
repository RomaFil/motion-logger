/**
 * @file iio_accel.c
 * @brief Реалізація драйвера акселерометра через IIO sysfs. Опис дивись у iio_accel.h.
 */
#include "iio_accel.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "io_util.h"

#define AXES 3
#define MSEC_PER_SEC 1000
#define NSEC_PER_MSEC 1000000L
#define MAX_RATE_HZ 1000
#define NUMBER_TEXT_SIZE 48

/* IIO віддає прискорення в м/с²; 1 g це стандартне прискорення вільного падіння. */
#define STANDARD_GRAVITY_MS2 9.80665

static const char *const AXIS_NAMES[AXES] = {"x", "y", "z"};

/* Приватні функції описано тут, а визначено внизу: публічна функція йде першою. */
static int read_accel(struct sensor *self, struct accel_sample *out);
static void close_accel(struct sensor *self);
static int build_path(char *out, const char *dir, const char *name);
static int read_number(const char *path, double *value);
static int read_raw(int fd, double *value);
static int read_scale(const char *dir, int axis, double *scale);
static int wait_until(int64_t due_ms);
static void close_all(struct iio_accel *dev);
static int read_name(const char *devices_dir, const char *entry, char *out, size_t size);

#define DEVICE_PREFIX "iio:device"
#define NAME_TEXT_SIZE 64

int iio_accel_open(struct iio_accel *dev, const char *device_dir, unsigned int rate_hz)
{
	char path[IIO_ACCEL_PATH_SIZE];
	char name[48];
	int axis;
	int result;

	if (dev == NULL || device_dir == NULL || rate_hz < 1 || rate_hz > MAX_RATE_HZ) {
		return -EINVAL;
	}

	for (axis = 0; axis < AXES; axis++) {
		dev->raw_fd[axis] = -1;
	}

	for (axis = 0; axis < AXES; axis++) {
		double scale_ms2;

		snprintf(name, sizeof(name), "in_accel_%s_raw", AXIS_NAMES[axis]);
		result = build_path(path, device_dir, name);
		if (result < 0) {
			goto fail;
		}
		dev->raw_fd[axis] = open(path, O_RDONLY | O_CLOEXEC);
		if (dev->raw_fd[axis] < 0) {
			result = -errno;
			goto fail;
		}

		result = read_scale(device_dir, axis, &scale_ms2);
		if (result < 0) {
			goto fail;
		}
		dev->g_per_count[axis] = scale_ms2 / STANDARD_GRAVITY_MS2;
	}

	dev->base.read = read_accel;
	dev->base.close = close_accel;
	dev->period_ms = MSEC_PER_SEC / (int64_t)rate_hz;
	dev->next_ms = 0;

	return 0;

fail:
	close_all(dev);
	return result;
}

int iio_accel_find(const char *devices_dir, const char *name, char *out, size_t size)
{
	struct dirent *entry;
	DIR *dir;
	int found = 0;
	int result = 0;

	if (devices_dir == NULL || name == NULL || name[0] == '\0' || out == NULL || size == 0) {
		return -EINVAL;
	}
	dir = opendir(devices_dir);
	if (dir == NULL) {
		return -errno;
	}

	while ((entry = readdir(dir)) != NULL) {
		char device_name[NAME_TEXT_SIZE];
		int length;

		if (strncmp(entry->d_name, DEVICE_PREFIX, strlen(DEVICE_PREFIX)) != 0) {
			continue;
		}
		if (read_name(devices_dir, entry->d_name, device_name, sizeof(device_name)) < 0 ||
		    strcmp(device_name, name) != 0) {
			continue;
		}
		found++;
		length = snprintf(out, size, "%s/%s", devices_dir, entry->d_name);
		if (length < 0 || (size_t)length >= size) {
			result = -ENAMETOOLONG;
		}
	}
	closedir(dir);

	if (found == 0) {
		return -ENODEV;
	}
	if (found > 1) {
		return -ENOTUNIQ;
	}

	return result;
}

/* Читає devices_dir/entry/name без кінцевого '\n'. */
static int read_name(const char *devices_dir, const char *entry, char *out, size_t size)
{
	char path[IIO_ACCEL_PATH_SIZE];
	ssize_t length;
	int fd;

	if (snprintf(path, sizeof(path), "%s/%s/name", devices_dir, entry) >= (int)sizeof(path)) {
		return -ENAMETOOLONG;
	}
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		return -errno;
	}
	length = read(fd, out, size - 1);
	close(fd);
	if (length < 0) {
		return -EIO;
	}
	out[length] = '\0';
	out[strcspn(out, "\n")] = '\0';

	return 0;
}

static int read_accel(struct sensor *self, struct accel_sample *out)
{
	struct iio_accel *dev = (struct iio_accel *)self; /* base це перше поле */
	double counts[AXES];
	int64_t now_ms;
	int axis;
	int result;

	if (dev->next_ms == 0) {
		dev->next_ms = monotonic_ms();
	}
	result = wait_until(dev->next_ms);
	if (result < 0) {
		return result; /* -EINTR: відлік ще не читали, нічого не втрачено */
	}

	now_ms = monotonic_ms();
	/* Наступний момент від попереднього, а не від "зараз": затримки не накопичуються.
	 * Якщо система відстала більше ніж на період, не наздоганяємо серією відліків підряд. */
	dev->next_ms += dev->period_ms;
	if (dev->next_ms < now_ms) {
		dev->next_ms = now_ms + dev->period_ms;
	}

	for (axis = 0; axis < AXES; axis++) {
		result = read_raw(dev->raw_fd[axis], &counts[axis]);
		if (result < 0) {
			return result;
		}
	}

	out->time_ms = now_ms;
	out->x_g = counts[0] * dev->g_per_count[0];
	out->y_g = counts[1] * dev->g_per_count[1];
	out->z_g = counts[2] * dev->g_per_count[2];

	return 0;
}

static void close_accel(struct sensor *self) { close_all((struct iio_accel *)self); }

static void close_all(struct iio_accel *dev)
{
	int axis;

	for (axis = 0; axis < AXES; axis++) {
		if (dev->raw_fd[axis] >= 0) {
			close(dev->raw_fd[axis]);
			dev->raw_fd[axis] = -1;
		}
	}
}

/* Склеює каталог і ім'я файлу; -EINVAL, якщо результат не влізає. */
static int build_path(char *out, const char *dir, const char *name)
{
	int length = snprintf(out, IIO_ACCEL_PATH_SIZE, "%s/%s", dir, name);

	if (length < 0 || length >= IIO_ACCEL_PATH_SIZE) {
		return -EINVAL;
	}

	return 0;
}

/* Читає один sysfs-файл з числом із початку файлу (pread: дескриптор лишається відкритим). */
static int read_raw(int fd, double *value)
{
	char text[NUMBER_TEXT_SIZE];
	char *end;
	ssize_t length = pread(fd, text, sizeof(text) - 1, 0);

	if (length <= 0) {
		return length < 0 ? -errno : -EIO;
	}
	text[length] = '\0';

	errno = 0;
	*value = strtod(text, &end);
	if (errno != 0 || end == text) {
		return -EIO;
	}

	return 0;
}

/* Відкриває файл на мить і читає з нього число (для масштабу, який читається один раз). */
static int read_number(const char *path, double *value)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	int result;

	if (fd < 0) {
		return -errno;
	}
	result = read_raw(fd, value);
	close(fd);

	return result;
}

/* Масштаб осі: спершу спільний in_accel_scale, інакше in_accel_<вісь>_scale. */
static int read_scale(const char *dir, int axis, double *scale)
{
	char path[IIO_ACCEL_PATH_SIZE];
	char name[48];
	int result;

	result = build_path(path, dir, "in_accel_scale");
	if (result < 0) {
		return result;
	}
	result = read_number(path, scale);
	if (result == -ENOENT) {
		snprintf(name, sizeof(name), "in_accel_%s_scale", AXIS_NAMES[axis]);
		result = build_path(path, dir, name);
		if (result < 0) {
			return result;
		}
		result = read_number(path, scale);
	}
	if (result < 0) {
		return result;
	}
	if (!isfinite(*scale) || *scale <= 0) {
		return -EPROTO;
	}

	return 0;
}

/* Спить до моменту due_ms (монотонний годинник). -EINTR, якщо сигнал перервав сон. */
static int wait_until(int64_t due_ms)
{
	struct timespec due;
	int error;

	due.tv_sec = due_ms / MSEC_PER_SEC;
	due.tv_nsec = (due_ms % MSEC_PER_SEC) * NSEC_PER_MSEC;

	error = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &due, NULL);
	if (error == 0) {
		return 0;
	}

	return error == EINTR ? -EINTR : -error;
}
