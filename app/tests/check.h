/**
 * @file check.h
 * @brief Спільна для тестів перевірка умов без фреймворків: звичайний C.
 *
 * Тест, який підключає цей файл, оголошує лічильник `static int failures;`
 * і повертає його як код виходу з main().
 */
#ifndef CHECK_H
#define CHECK_H

#include <stdio.h>

/*
 * Перевіряє умову. Якщо вона хибна, друкує файл, рядок і сам вираз та рахує помилку.
 * Тест після цього не зупиняється: так за один запуск видно всі проблеми.
 */
#define CHECK(condition)                                                                           \
	do {                                                                                       \
		if (!(condition)) {                                                                \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);                \
			failures++;                                                                \
		}                                                                                  \
	} while (0)

#endif /* CHECK_H */
