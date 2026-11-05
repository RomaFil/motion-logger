/**
 * @file utc_time.h
 * @brief Дата й час UTC: перетворення між календарною датою та секундами з 1970-01-01.
 *
 * GPS дає дату й час UTC окремими полями, а на Raspberry Pi 5 немає батарейки годинника, тож
 * системний час може бути невірним. Тут чисті функції без доступу до годинника й локалі
 * (timegm і gmtime_r не потрібні, поведінка однакова на всіх платформах і легко перевіряється).
 */
#ifndef UTC_TIME_H
#define UTC_TIME_H

#include <stddef.h>
#include <stdint.h>

/** @brief Мінімальна довжина буфера для utc_time_format (з нулем): "2026-10-28T12:00:08Z". */
#define UTC_TIME_TEXT_SIZE 21

/**
 * @brief Секунди з 1970-01-01T00:00:00Z для дати й секунд від початку доби.
 *
 * Григоріанський календар, високосні роки враховано. Перевірок діапазону не робить (дату вже
 * перевірив парсер NMEA): день 1..31, місяць 1..12.
 *
 * @param seconds_of_day 0..86399 (60-та секунда високосної секунди дає перехід на наступну хвилину)
 */
int64_t utc_time_to_epoch(int year, int month, int day, uint32_t seconds_of_day);

/**
 * @brief Записує час у вигляді "YYYY-MM-DDThh:mm:ssZ".
 *
 * @param out   куди писати
 * @param size  розмір out, не менше UTC_TIME_TEXT_SIZE
 * @param epoch секунди з 1970-01-01T00:00:00Z (не менше нуля і не більше 253402300799, тобто рік
 * 9999)
 * @return довжина рядка без нуля; -1, якщо out NULL, буфер малий чи час поза діапазоном
 */
int utc_time_format(char *out, size_t size, int64_t epoch);

#endif /* UTC_TIME_H */
