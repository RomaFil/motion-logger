/**
 * @file uart_port.c
 * @brief Реалізація відкриття послідовного порту. Опис дивись у uart_port.h.
 */
#include "uart_port.h"

#include <errno.h>
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>

static speed_t baud_to_speed(unsigned int baud);

int uart_port_open(const char *path, unsigned int baud)
{
	struct termios tio;
	speed_t speed = baud_to_speed(baud);
	int error;
	int fd;

	if (path == NULL || speed == B0) {
		return -EINVAL;
	}

	/* O_NOCTTY: порт не стає керівним терміналом процесу; O_NONBLOCK: читач чекає через epoll.
	 */
	fd = open(path, O_RDONLY | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0) {
		return -errno;
	}

	if (tcgetattr(fd, &tio) < 0) {
		if (errno == ENOTTY) {
			return fd; /* не термінал (FIFO чи файл): налаштовувати нічого */
		}
		goto fail;
	}

	/* Raw-режим: без обробки рядка, ехо й перетворення \r\n, читаємо байти як є. */
	tio.c_iflag = 0;
	tio.c_oflag = 0;
	tio.c_lflag = 0;
	/* 8 біт, один стоп-біт, без парності й апаратного керування потоком; ігнорувати лінії
	 * модема (CLOCAL), вмикнути приймач (CREAD). */
	tio.c_cflag = CS8 | CLOCAL | CREAD;
	tio.c_cc[VMIN] = 0;
	tio.c_cc[VTIME] = 0;
	if (cfsetispeed(&tio, speed) < 0 || cfsetospeed(&tio, speed) < 0 ||
	    tcsetattr(fd, TCSANOW, &tio) < 0) {
		goto fail;
	}
	tcflush(fd, TCIFLUSH); /* викинути байти, що накопичились до налаштування */

	return fd;

fail:
	error = -errno;
	close(fd);

	return error;
}

static speed_t baud_to_speed(unsigned int baud)
{
	switch (baud) {
	case 4800:
		return B4800;
	case 9600:
		return B9600;
	case 19200:
		return B19200;
	case 38400:
		return B38400;
	case 57600:
		return B57600;
	case 115200:
		return B115200;
	default:
		return B0;
	}
}
