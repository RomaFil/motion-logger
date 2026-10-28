/**
 * @file io_util.c
 * @brief Реалізація спільних функцій введення-виведення. Опис дивись у io_util.h.
 */
#include "io_util.h"

#include <errno.h>
#include <time.h>
#include <unistd.h>

#define MSEC_PER_SEC 1000
#define NSEC_PER_MSEC 1000000L

int write_all(int fd, const void *data, size_t length)
{
	const char *cursor = data;

	while (length > 0) {
		ssize_t written = write(fd, cursor, length);

		if (written < 0) {
			if (errno == EINTR) {
				continue;
			}
			return -errno;
		}
		cursor += written;
		length -= (size_t)written;
	}

	return 0;
}

int64_t monotonic_ms(void)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);

	return (int64_t)now.tv_sec * MSEC_PER_SEC + now.tv_nsec / NSEC_PER_MSEC;
}
