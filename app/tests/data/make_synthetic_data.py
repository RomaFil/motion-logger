#!/usr/bin/env python3
"""Генератор СИНТЕТИЧНИХ (вигаданих) тестових даних для Motion Logger.

Створює три файли в каталозі tests/data/:
  accel_sample.csv   відліки акселерометра: t_ms,x_g,y_g,z_g (100 відліків на секунду)
  gps_sample.nmea    рядки NMEA від "GPS-модуля" (1 Гц), з холодним стартом і двома збитими рядками
  ground_truth.txt   що "відбувалось" насправді: проміжки "still" (стою) і "walking" (іду)

Дані НЕ записані з плати. Вони потрібні, щоб конвеєр обробки (крок 3.5) працював і
перевірявся до того, як з'являться справжні записи. Справжні файли кладуться на місце
цих з тими самими іменами й форматом; ground_truth.txt переписується за тим, що
робив Роман під час запису. Дивись tests/data/README.md.

Запуск: python3 make_synthetic_data.py   (результат завжди однаковий: зерно фіксоване)
"""

import math
import os
import random

SEED = 20261028
SAMPLE_RATE_HZ = 100
STEPS_PER_SECOND = 2.0
REST_NOISE_G = 0.006  # припущення про шум датчика на столі, не виміряне
WALK_NOISE_G = 0.02

# (початок, кінець, стан) у секундах
SEGMENTS = [(0, 20, "still"), (20, 40, "walking"), (40, 50, "still")]
TOTAL_SECONDS = SEGMENTS[-1][1]

# Плата лежить трохи нахилено: вектор тяжіння не точно по Z. Довжина вектора 1 g.
TILT = (0.12, -0.05)

# Вигадана позиція, центр Києва (не прив'язана до справжнього запису).
START_LATITUDE = 50.4501
START_LONGITUDE = 30.5234
FIX_SECOND = 8  # до цієї секунди "супутників ще нема" (холодний старт)
SPEED_KNOTS_WALKING = 2.5
METERS_PER_DEGREE_LATITUDE = 111_000.0
KNOT_IN_MPS = 0.514444
BAD_CHECKSUM_SECONDS = (15, 33)


def gravity_direction():
    x, y = TILT
    z = math.sqrt(1.0 - x * x - y * y)
    return x, y, z


def state_at(second):
    for start, end, state in SEGMENTS:
        if start <= second < end:
            return state
    raise ValueError(second)


def write_accel(path, rng):
    gx, gy, gz = gravity_direction()
    phase = rng.uniform(0, 2 * math.pi)
    lines = [
        "# SYNTHETIC DATA: вигадані дані, НЕ записані з плати (tests/data/make_synthetic_data.py).",
        "# Формат: t_ms,x_g,y_g,z_g (прискорення в g, 100 відліків на секунду).",
        "# Справжній запис кладеться на місце цього файлу з тим самим форматом.",
    ]
    total = TOTAL_SECONDS * SAMPLE_RATE_HZ
    for i in range(total):
        t = i / SAMPLE_RATE_HZ
        if state_at(int(t)) == "still":
            ax = gx + rng.gauss(0, REST_NOISE_G)
            ay = gy + rng.gauss(0, REST_NOISE_G)
            az = gz + rng.gauss(0, REST_NOISE_G)
        else:
            step = math.sin(2 * math.pi * STEPS_PER_SECOND * t + phase)
            sway = math.sin(math.pi * STEPS_PER_SECOND * t + phase)
            scale = 1.0 + 0.35 * step
            ax = gx * scale + 0.12 * sway + rng.gauss(0, WALK_NOISE_G)
            ay = gy * scale + 0.08 * step + rng.gauss(0, WALK_NOISE_G)
            az = gz * scale + rng.gauss(0, WALK_NOISE_G)
        lines.append(f"{i * 1000 // SAMPLE_RATE_HZ},{ax:.4f},{ay:.4f},{az:.4f}")
    open(path, "w", encoding="utf-8", newline="\n").write("\n".join(lines) + "\n")


def nmea_checksum(body):
    value = 0
    for char in body:
        value ^= ord(char)
    return f"{value:02X}"


def sentence(body, corrupt=False):
    checksum = nmea_checksum(body)
    if corrupt:
        # Псуємо один символ тіла, а суму лишаємо від справжнього тіла: як перешкода на дроті.
        index = body.index(",") + 3
        broken = body[:index] + ("0" if body[index] != "0" else "1") + body[index + 1:]
        return f"${broken}*{checksum}\r\n"
    return f"${body}*{checksum}\r\n"


def to_ddmm(value, width):
    degrees = int(value)
    minutes = (value - degrees) * 60.0
    return f"{degrees:0{width}d}{minutes:07.4f}"


def write_gps(path):
    latitude = START_LATITUDE
    longitude = START_LONGITUDE
    out = []
    out.append(sentence("GPTXT,01,01,02,u-blox ag - www.u-blox.com"))
    for second in range(TOTAL_SECONDS):
        hh, mm, ss = 12, second // 60, second % 60
        time_field = f"{hh:02d}{mm:02d}{ss:02d}.00"
        has_fix = second >= FIX_SECOND
        walking = state_at(second) == "walking"
        speed = SPEED_KNOTS_WALKING if walking else 0.0
        if has_fix and walking:
            latitude += SPEED_KNOTS_WALKING * KNOT_IN_MPS / METERS_PER_DEGREE_LATITUDE
        if has_fix:
            lat = to_ddmm(latitude, 2)
            lon = to_ddmm(longitude, 3)
            gga = f"GPGGA,{time_field},{lat},N,{lon},E,1,07,1.2,180.0,M,28.0,M,,"
            rmc = f"GPRMC,{time_field},A,{lat},N,{lon},E,{speed:.1f},90.0,281026,,,A"
        else:
            gga = f"GPGGA,{time_field},,,,,0,00,99.99,,,,,,"
            rmc = f"GPRMC,{time_field},V,,,,,,,281026,,,N"
        corrupt = second in BAD_CHECKSUM_SECONDS
        out.append(sentence(gga, corrupt=corrupt))
        out.append(sentence(rmc))
        if second % 5 == 0:
            out.append(sentence("GPGSV,1,1,00" if not has_fix else "GPGSV,1,1,07,01,40,083,41"))
    open(path, "w", encoding="ascii", newline="").write("".join(out))


def write_truth(path):
    lines = [
        "# SYNTHETIC: що відбувалось насправді. Рядок: початок_с кінець_с стан (still|walking).",
        "# Для справжнього запису перепиши за тим, що робив: 0 60 still / 60 120 walking.",
    ]
    for start, end, state in SEGMENTS:
        lines.append(f"{start} {end} {state}")
    open(path, "w", encoding="utf-8", newline="\n").write("\n".join(lines) + "\n")


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    rng = random.Random(SEED)
    write_accel(os.path.join(here, "accel_sample.csv"), rng)
    write_gps(os.path.join(here, "gps_sample.nmea"))
    write_truth(os.path.join(here, "ground_truth.txt"))


if __name__ == "__main__":
    main()
