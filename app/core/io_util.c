/**
 * @file io_util.c
 * @brief Реалізація спільних функцій введення-виведення. Опис дивись у io_util.h.
 */
#include "io_util.h"

#include <errno.h>
#include <unistd.h>

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
