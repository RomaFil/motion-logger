/**
 * @file gpio_lines.c
 * @brief Реалізація ліній GPIO через uAPI v2. Опис дивись у gpio_lines.h.
 */
#include "gpio_lines.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/gpio.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define NSEC_PER_MSEC 1000000ULL

static int request_lines(const char *chip_path, const unsigned int *offsets, unsigned int count,
			 const char *consumer, struct gpio_v2_line_config *config);
static uint64_t all_lines_mask(unsigned int count);

int gpio_lines_request_outputs(const char *chip_path, const unsigned int *offsets,
			       unsigned int count, const char *consumer)
{
	struct gpio_v2_line_config config;

	memset(&config, 0, sizeof(config));
	config.flags = GPIO_V2_LINE_FLAG_OUTPUT;

	return request_lines(chip_path, offsets, count, consumer, &config);
}

int gpio_lines_set(int fd, uint64_t bits, uint64_t mask)
{
	struct gpio_v2_line_values values = {.bits = bits, .mask = mask};

	if (ioctl(fd, GPIO_V2_LINE_SET_VALUES_IOCTL, &values) < 0) {
		return -errno;
	}

	return 0;
}

int gpio_lines_request_buttons(const char *chip_path, const unsigned int *offsets,
			       unsigned int count, bool active_low, unsigned int debounce_us,
			       const char *consumer)
{
	struct gpio_v2_line_config config;

	memset(&config, 0, sizeof(config));
	/* Фронт рахується для логічного значення: з ACTIVE_LOW спад рівня це «стала активною». */
	config.flags = GPIO_V2_LINE_FLAG_INPUT | GPIO_V2_LINE_FLAG_EDGE_RISING;
	if (active_low) {
		config.flags |= GPIO_V2_LINE_FLAG_ACTIVE_LOW | GPIO_V2_LINE_FLAG_BIAS_PULL_UP;
	} else {
		config.flags |= GPIO_V2_LINE_FLAG_BIAS_PULL_DOWN;
	}
	if (debounce_us > 0) {
		config.num_attrs = 1;
		config.attrs[0].attr.id = GPIO_V2_LINE_ATTR_ID_DEBOUNCE;
		config.attrs[0].attr.debounce_period_us = debounce_us;
		config.attrs[0].mask = all_lines_mask(count);
	}

	return request_lines(chip_path, offsets, count, consumer, &config);
}

int gpio_lines_read_event(int fd, unsigned int *offset, int64_t *timestamp_ms)
{
	struct gpio_v2_line_event event;
	ssize_t length;

	if (offset == NULL || timestamp_ms == NULL) {
		return -EINVAL;
	}
	length = read(fd, &event, sizeof(event));
	if (length < 0) {
		return -errno;
	}
	if ((size_t)length != sizeof(event)) {
		return -EIO;
	}
	*offset = event.offset;
	*timestamp_ms = (int64_t)(event.timestamp_ns / NSEC_PER_MSEC);

	return 0;
}

/* Один запит на всі лінії: дескриптор чипа після нього вже не потрібен. */
static int request_lines(const char *chip_path, const unsigned int *offsets, unsigned int count,
			 const char *consumer, struct gpio_v2_line_config *config)
{
	struct gpio_v2_line_request request;
	unsigned int i;
	int chip_fd;
	int error;

	if (chip_path == NULL || offsets == NULL || consumer == NULL || count == 0 ||
	    count > GPIO_LINES_MAX) {
		return -EINVAL;
	}

	memset(&request, 0, sizeof(request));
	for (i = 0; i < count; i++) {
		request.offsets[i] = offsets[i];
	}
	request.num_lines = count;
	request.config = *config;
	snprintf(request.consumer, sizeof(request.consumer), "%s", consumer);

	chip_fd = open(chip_path, O_RDWR | O_CLOEXEC);
	if (chip_fd < 0) {
		return -errno;
	}
	if (ioctl(chip_fd, GPIO_V2_GET_LINE_IOCTL, &request) < 0) {
		error = -errno;
		close(chip_fd);
		return error;
	}
	close(chip_fd);

	return request.fd;
}

static uint64_t all_lines_mask(unsigned int count)
{
	return count >= GPIO_LINES_MAX ? UINT64_MAX : (UINT64_C(1) << count) - 1;
}
