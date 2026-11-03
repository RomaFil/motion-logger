/**
 * @file test_gpio_lines.c
 * @brief Перевірки драйвера gpio_lines без заліза: лише відмова на поганих параметрах і не-чипі.
 *
 * Справжні лінії перевіряються на платі (README, «Світлодіоди й кнопки»): на комп'ютері розробника
 * /dev/gpiochipN зазвичай нема, а gpio-sim потребує root і модуля ядра.
 *
 * Запуск: make test (звичайний) або make test-tsan. Код виходу 0 означає, що все пройшло.
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "check.h"
#include "gpio_lines.h"

/* Скільки перевірок не пройшло. main повертає це як код виходу. */
static int failures;

static void test_bad_arguments_are_rejected(void)
{
	static const unsigned int lines[1] = {17};
	unsigned int offset;
	int64_t time_ms;

	CHECK(gpio_lines_request_outputs(NULL, lines, 1, "t") == -EINVAL);
	CHECK(gpio_lines_request_outputs("/dev/gpiochip0", NULL, 1, "t") == -EINVAL);
	CHECK(gpio_lines_request_outputs("/dev/gpiochip0", lines, 0, "t") == -EINVAL);
	CHECK(gpio_lines_request_outputs("/dev/gpiochip0", lines, GPIO_LINES_MAX + 1, "t") ==
	      -EINVAL);
	CHECK(gpio_lines_request_buttons("/dev/gpiochip0", lines, 1, true, 0, NULL) == -EINVAL);
	CHECK(gpio_lines_read_event(0, NULL, &time_ms) == -EINVAL);
	CHECK(gpio_lines_read_event(0, &offset, NULL) == -EINVAL);
}

static void test_missing_chip_is_reported(void)
{
	static const unsigned int lines[2] = {17, 13};

	CHECK(gpio_lines_request_outputs("/no/such/gpiochip", lines, 2, "t") == -ENOENT);
	CHECK(gpio_lines_request_buttons("/no/such/gpiochip", lines, 2, true, 30000, "t") ==
	      -ENOENT);
}

/* Звичайний файл замість чипа: ioctl відмовляє (ENOTTY), дескриптор не тече. */
static void test_regular_file_is_not_a_chip(void)
{
	static const unsigned int lines[1] = {27};
	char path[] = "/tmp/test_gpio_XXXXXX";
	int fd = mkstemp(path);

	CHECK(fd >= 0);
	close(fd);
	CHECK(gpio_lines_request_outputs(path, lines, 1, "t") == -ENOTTY);
	CHECK(gpio_lines_set(-1, 1, 1) == -EBADF);
	unlink(path);
}

int main(void)
{
	test_bad_arguments_are_rejected();
	test_missing_chip_is_reported();
	test_regular_file_is_not_a_chip();

	if (failures != 0) {
		printf("%d check(s) failed\n", failures);
		return 1;
	}
	printf("gpio_lines: all checks passed\n");

	return 0;
}
