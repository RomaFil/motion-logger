/**
 * @file gpio_lines.h
 * @brief Лінії GPIO через символьний пристрій ядра (/dev/gpiochipN, uAPI v2): світлодіоди й кнопки.
 *
 * Без libgpiod і без застарілого /sys/class/gpio: один ioctl GPIO_V2_GET_LINE_IOCTL просить у ядра
 * кілька ліній одразу й повертає на них дескриптор. Для виходів через нього ставляться значення,
 * для входів з нього читаються події фронтів (struct gpio_v2_line_event) з міткою часу ядра на
 * монотонному годиннику; дескриптор вміє epoll. Брязкіт контактів кнопки гасить саме ядро
 * (атрибут debounce).
 *
 * Номери ліній це номери GPIO на гребінці Pi 5 (GPIO17, GPIO27…): на Raspberry Pi 5 з ядром 6.6+
 * вони належать /dev/gpiochip0 (RP1, мітка pinctrl-rp1). Перевірка на платі: gpioinfo gpiochip0.
 */
#ifndef GPIO_LINES_H
#define GPIO_LINES_H

#include <stdbool.h>
#include <stdint.h>

/** @brief Найбільше ліній в одному запиті (обмеження ядра GPIO_V2_LINES_MAX = 64). */
#define GPIO_LINES_MAX 64

/**
 * @brief Просить лінії як виходи, усі в стані 0.
 *
 * @param chip_path наприклад /dev/gpiochip0
 * @param offsets   номери ліній на чипі
 * @param count     скільки ліній, 1..GPIO_LINES_MAX
 * @param consumer  ім'я, яке видно в gpioinfo
 * @return дескриптор ліній (закрити close()); -EINVAL при поганих параметрах; -EBUSY, якщо
 *         лінію вже зайняв хтось інший; інакше мінус errno відкриття чи ioctl
 */
int gpio_lines_request_outputs(const char *chip_path, const unsigned int *offsets,
			       unsigned int count, const char *consumer);

/**
 * @brief Ставить значення виходів: біт i у bits це лінія offsets[i]; змінюються лише біти з mask.
 * @return 0 або мінус errno
 */
int gpio_lines_set(int fd, uint64_t bits, uint64_t mask);

/**
 * @brief Просить лінії як входи кнопок з подіями «натиснуто».
 *
 * active_low: кнопка замикає лінію на землю (внутрішня підтяжка вгору), натискання це спад
 * рівня. Інакше кнопка дає 3,3 В (підтяжка вниз), натискання це зростання рівня.
 *
 * @param debounce_us скільки мікросекунд рівень має триматися, щоб ядро повірило (0 = без)
 * @return дескриптор ліній; коди помилок як у gpio_lines_request_outputs()
 */
int gpio_lines_request_buttons(const char *chip_path, const unsigned int *offsets,
			       unsigned int count, bool active_low, unsigned int debounce_us,
			       const char *consumer);

/**
 * @brief Читає одну подію натискання (викликати, коли epoll сказав, що дескриптор готовий).
 *
 * @param offset       номер лінії, на якій натиснули
 * @param timestamp_ms мітка часу ядра, монотонний годинник, мілісекунди
 * @return 0; -EAGAIN, якщо подій нема; інакше мінус errno
 */
int gpio_lines_read_event(int fd, unsigned int *offset, int64_t *timestamp_ms);

#endif /* GPIO_LINES_H */
