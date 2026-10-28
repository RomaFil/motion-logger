#!/usr/bin/env python3
"""Записує акселерометр MPU6050 (і за бажанням рядки NMEA від GPS) у файли для кроків 1.6 і 3.5.

Результат лягає у формат, який читають accel_csv, csv_player і analyze_recording:
    # коментарі
    t_ms,x_g,y_g,z_g          (t_ms від початку запису, g)
і у ground_truth.txt (рядки "початок кінець мітка", секунди), за яким check_samples.sh
звіряє, де фільтр руху сказав "рух".

Запуск на Pi (картка Pi OS, пакет python3-smbus2):
    python3 scripts/record_accel.py --out app/tests/data --phases still:60,walking:60 --gps /dev/ttyAMA2
Без плати (перевірка сценарію): додайте --fake, тоді дані синтетичні.

Скрипт сам підказує, що робити в кожну фазу ("лежить", "ходьба"). Фази задаються як мітка:секунди;
мітки still і walking зрозумілі check_samples.sh, інші теж дозволені.
"""
import argparse
import math
import os
import sys
import threading
import time

MPU_ADDR = 0x68
REG_PWR_MGMT_1 = 0x6B
REG_ACCEL_CONFIG = 0x1C
REG_ACCEL_XOUT_H = 0x3B
LSB_PER_G_2G = 16384.0  # діапазон ±2 g (ACCEL_CONFIG = 0), значення з документації датчика
STANDARD_GRAVITY = 1.0


def parse_phases(text):
    """'still:60,walking:60' -> [('still', 60.0), ('walking', 60.0)]"""
    phases = []
    for item in text.split(","):
        label, _, seconds = item.partition(":")
        label = label.strip()
        if not label or not seconds:
            raise ValueError("фаза має вигляд мітка:секунди, отримано %r" % item)
        value = float(seconds)
        if value <= 0:
            raise ValueError("тривалість фази має бути додатною: %r" % item)
        phases.append((label, value))
    if not phases:
        raise ValueError("не задано жодної фази")
    return phases


def to_signed16(high, low):
    value = (high << 8) | low
    return value - 0x10000 if value & 0x8000 else value


def decode_accel(raw6):
    """6 байтів регістрів ACCEL_XOUT_H.. -> (x_g, y_g, z_g)."""
    return tuple(
        to_signed16(raw6[i], raw6[i + 1]) / LSB_PER_G_2G for i in (0, 2, 4)
    )


def open_mpu6050(bus_number, address):
    """Повертає функцію read(), що віддає (x_g, y_g, z_g) з MPU6050."""
    from smbus2 import SMBus  # імпорт тут, щоб --fake працював без пакета

    bus = SMBus(bus_number)
    bus.write_byte_data(address, REG_PWR_MGMT_1, 0x00)  # вийти зі сну
    time.sleep(0.1)
    bus.write_byte_data(address, REG_ACCEL_CONFIG, 0x00)  # ±2 g

    def read():
        return decode_accel(bus.read_i2c_block_data(address, REG_ACCEL_XOUT_H, 6))

    return read


def make_fake(phases_by_time):
    """Синтетичний датчик для сухого прогону: у фазі walking коливання по Z ≈ 0,35 g, 2 кроки/с."""

    def read(t_seconds, label):
        z = STANDARD_GRAVITY
        if label == "walking":
            z += 0.35 * math.sin(2 * math.pi * 2 * t_seconds)
        return (0.0, 0.0, z)

    return read


def start_gps_capture(path, out_path, stop_event):
    """Читає рядки NMEA з послідовного порту (9600 8N1) і пишe їх як є (CRLF зберігається)."""
    import termios

    fd = os.open(path, os.O_RDONLY | os.O_NOCTTY)
    attrs = termios.tcgetattr(fd)
    attrs[0] = 0  # iflag
    attrs[1] = 0  # oflag
    attrs[2] = termios.CS8 | termios.CREAD | termios.CLOCAL  # cflag
    attrs[3] = 0  # lflag
    attrs[4] = attrs[5] = termios.B9600
    attrs[6][termios.VMIN] = 0
    attrs[6][termios.VTIME] = 5  # 0,5 с, щоб перевіряти stop_event
    termios.tcsetattr(fd, termios.TCSANOW, attrs)

    def run():
        with open(out_path, "wb") as out:
            while not stop_event.is_set():
                chunk = os.read(fd, 256)
                if chunk:
                    out.write(chunk)
        os.close(fd)

    thread = threading.Thread(target=run, daemon=True)
    thread.start()
    return thread


def record(args):
    phases = parse_phases(args.phases)
    os.makedirs(args.out, exist_ok=True)
    csv_path = os.path.join(args.out, "accel_sample.csv")
    truth_path = os.path.join(args.out, "ground_truth.txt")
    gps_path = os.path.join(args.out, "gps_sample.nmea")

    if args.fake:
        fake = make_fake(phases)
        read_sample = None
    else:
        read_sample = open_mpu6050(args.bus, args.address)

    stop_gps = threading.Event()
    gps_thread = None
    if args.gps:
        gps_thread = start_gps_capture(args.gps, gps_path, stop_gps)

    period = 1.0 / args.rate
    total = sum(seconds for _, seconds in phases)
    boundaries = []
    start = 0.0
    for label, seconds in phases:
        boundaries.append((start, start + seconds, label))
        start += seconds

    print("Запис %.0f с, %g Гц. Файли: %s" % (total, args.rate, args.out))
    for _, _, label in boundaries[:1]:
        print(">>> фаза: %s" % label)
    current = 0

    t0 = time.monotonic()
    samples = 0
    with open(csv_path, "w", encoding="utf-8", newline="\n") as csv:
        mode = "FAKE (синтетичні дані)" if args.fake else "REAL (MPU6050)"
        csv.write("# %s, %g Гц, діапазон +-2 g, формат t_ms,x_g,y_g,z_g\n" % (mode, args.rate))
        csv.write("# фази: %s\n" % ", ".join("%s %gс" % p for p in phases))
        n = 0
        while True:
            now = time.monotonic() - t0
            if now >= total:
                break
            while current < len(boundaries) - 1 and now >= boundaries[current][1]:
                current += 1
                print(">>> фаза: %s" % boundaries[current][2])
            if args.fake:
                x, y, z = fake(now, boundaries[current][2])
            else:
                x, y, z = read_sample()
            csv.write("%d,%.5f,%.5f,%.5f\n" % (round(now * 1000), x, y, z))
            samples += 1
            n += 1
            sleep_for = t0 + n * period - time.monotonic()
            if sleep_for > 0:
                time.sleep(sleep_for)

    stop_gps.set()
    if gps_thread is not None:
        gps_thread.join(timeout=2)

    with open(truth_path, "w", encoding="utf-8", newline="\n") as truth:
        truth.write("# SYNTHETIC\n" if args.fake else "# REAL: записано scripts/record_accel.py\n")
        for begin, end, label in boundaries:
            truth.write("%d %d %s\n" % (round(begin), round(end), label))

    print("Готово: %d відліків у %s" % (samples, csv_path))
    print("Далі: cd app && make update-sample-report && make check-samples (див. app/tests/data/README.md)")
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--out", default="app/tests/data", help="каталог для accel_sample.csv, gps_sample.nmea, ground_truth.txt")
    parser.add_argument("--phases", default="still:60,walking:60", help="мітка:секунди через кому, наприклад still:60,walking:60")
    parser.add_argument("--rate", type=float, default=100.0, help="частота відліків, Гц (за замовчуванням 100)")
    parser.add_argument("--bus", type=int, default=1, help="номер I2C-шини (за замовчуванням 1)")
    parser.add_argument("--address", type=lambda s: int(s, 0), default=MPU_ADDR, help="адреса MPU6050 (за замовчуванням 0x68)")
    parser.add_argument("--gps", default="", help="послідовний порт GPS, наприклад /dev/ttyAMA2; без нього NMEA не записується")
    parser.add_argument("--fake", action="store_true", help="синтетичні дані замість датчика (перевірка сценарію без плати)")
    args = parser.parse_args(argv)
    if args.rate <= 0 or args.rate > 1000:
        parser.error("--rate має бути в межах (0; 1000]")
    try:
        return record(args)
    except ValueError as error:
        parser.error(str(error))


if __name__ == "__main__":
    sys.exit(main())
