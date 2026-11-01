# Протокол перевірки Motion Logger на платі (01.11.2026)

Плата: Raspberry Pi 5 Model B Rev 1.1 + наша плата-шилд (MPU6050, ADXL345, GPS NEO-6M). Образ: власний Buildroot 2026.08,
ядро 6.12.61-v8-16k, гілка `feature/wifi`. Мережа: лише Wi-Fi (Ethernet недоступний), `wlan0` = `192.168.0.240`.

## Wi-Fi і доступ

| № | Тест | Команда | Очікувано | Факт | Висновок |
|---|---|---|---|---|---|
| W1 | Драйвер і прошивка Wi-Fi | `dmesg \| grep brcmfmac` | чип визначено, прошивка завантажена | `BCM4345/6 wl0 version 7.45.234`, `wlan0` з'явився; помилка `…raspberrypi,5-model-b.bin -2` (драйвер бере загальну `brcmfmac43455-sdio.bin`) | пройшов, помилка не шкодить |
| W2 | Підключення до роутера (перший образ) | `wpa_cli status`, `wifi-debug.log` | `wpa_state=COMPLETED`, пінг з ПК | `ASSOCIATED`, далі `Authentication timed out` по колу, адреса є, зв'язок рветься | **не пройшов**, причина знайдена |
| W3 | Виправлення W2 | `brcmfmac.feature_disable=0x82000` (cmdline, потім `modprobe.d`) | стабільний зв'язок | 20 з 20 пінгів (4-33 мс), SSH відкритий, сигнал -34 dBm | пройшов |
| W4 | SSH за ключем | `ssh root@192.168.0.240 uname -a` | вхід без пароля | `Linux buildroot 6.12.61-v8-16k aarch64` | пройшов |
| W5 | `power_save` вимкнено | `iw dev wlan0 get power_save` | `off` | `Power save: off` (у знімку `wifi-debug.log`) | пройшов |

## Датчики і GPS

| № | Тест | Команда | Очікувано | Факт | Висновок |
|---|---|---|---|---|---|
| S1 | Модулі ядра | `lsmod` | `inv_mpu6050_i2c`, `adxl345_spi`, `i2c_dev`, `spidev`, `spi_dw_mmio`, `i2c_designware_platform` | усі завантажені | пройшов |
| S2 | IIO-пристрої | `cat /sys/bus/iio/devices/iio:device*/name` | `mpu6050` і `adxl345` | `iio:device0` = `mpu6050` (`i2c-1/1-0068`), `iio:device1` = `adxl345` (`spi0.0`) | пройшов, номери записано |
| S3 | I2C-шина | `i2cdetect -y 1` | `0x68` = `UU` | `0x68` = `UU`; додатково `0x1e` і `0x77` | пройшов; 0x1e (магнітометр) і 0x77 (барометр) поки без драйвера |
| S4 | Вузли пристроїв | `ls /dev/i2c-1 /dev/spidev* /dev/ttyAMA2` | є `i2c-1`, `ttyAMA2`, `spidev0.0` вимкнено | `i2c-1`, `ttyAMA2` є; `spidev0.0` відсутній (як задумано), є `spidev0.1` | пройшов |
| S5 | Частота MPU6050 | `cat sampling_frequency`; `echo 100 > …` | ≥ 100 Гц | було 50, запис 100 прийнято, прочитано 100 | пройшов після запису (за замовчуванням 50) |
| S6 | Частота ADXL345 | `cat in_accel_sampling_frequency` | ≥ 100 Гц | `100.0` (атрибут називається `in_accel_sampling_frequency`, не `sampling_frequency`) | пройшов |
| S7 | `in_accel_*_raw` оновлюються | 10 читань по 100 мс | значення змінюються | MPU `x≈3300 y≈150 z≈15150`, ADXL `x≈35 y≈-3 z≈260`, шум є; z ≈ 1 g на обох | пройшов; повний нахил руками не перевірявся |
| S8 | NMEA від GPS | `stty -F /dev/ttyAMA2 9600 raw; cat /dev/ttyAMA2` | рядки NMEA | 48 рядків за 8 с (GPRMC, GPVTG, GPGGA, GPGSA, GPGSV, GPGLL) | пройшов; фіксації в приміщенні нема (`V`, 0 супутників) |

## Застосунок

| № | Тест | Команда | Очікувано | Факт | Висновок |
|---|---|---|---|---|---|
| A1 | Застосунок у образі | `ls -l /usr/bin/motion_logger; motion_logger --help` | повний застосунок з `--iio`, `--gps` | 10016 байт, виводить `skeleton OK` (стара збірка від 15.10) | **не пройшов**, причина: `local`-пакет Buildroot не перебудувався |
| A2 | Те саме після `make motion_logger-dirclean` | те саме | повна довідка | 39448 байт, довідка з `--iio`, `--iio-aux`, `--gps`, `--log` | пройшов (у новому образі застосунок свіжий) |
| A3 | 10 секунд, три джерела | `motion_logger --iio …device0 --iio-rate 100 --iio-aux …device1 --gps /dev/ttyAMA2 --log /tmp/m.jsonl` | рядки `accel`, `accel2`, `gps`, `dropped 0`, `log errors 0` | `received 2004, dropped 0, logged 32 (events 1, rows 10, aux 11, gps 10), log errors 0`; `t_ms` зростає від 541917 до 551917; вихід 0 | пройшов |

## Знайдені проблеми

1. **wpa_supplicant 2.12 і brcmfmac**: після асоціації `wpa_supplicant` чекає від драйвера `port authorized` і через 10 с розриває
   з'єднання. Обхід: `options brcmfmac feature_disable=0x82000` (`etc/modprobe.d/brcmfmac.conf`). Докладно в `docs/wifi.md`.
2. **Застарілий застосунок в образах до 01.11.2026**: пакет з `SITE_METHOD = local` не перебудовується сам, коли змінюється
   `app/`; усі образи до цього містили 13-кілобайтну заготовку. Лікування: `make motion_logger-dirclean`, перед збіркою образу.
3. **Країна `UA` у `wpa_supplicant`** з прошивкою з `linux-firmware` викликала нескінченні `REG_CHANGE`; тому за замовчуванням країна не задається.
4. **MPU6050 стартує на 50 Гц**: для 100 Гц і вище частоту треба записувати (`echo 100 > sampling_frequency`).
5. Шум у журналі ядра: `brcmf_set_channel … reason -52` на каналах 5 ГГц і 12-14 (світовий регдомен), на роботу не впливає.
6. Годинника на платі немає (дата 1970), лог-файл на `/boot` має мітку 1980.

## Не перевірено

- Запис крок 1.6 (спокій 60 с і ходьба 60 с з GPS на вулиці): потрібен Pi OS, `scripts/record_accel.py` і відкрите небо.
- Нахил плати руками для перевірки `in_accel_*_raw` по осях, `HMC5883L` (0x1e) і `BMP/BME` (0x77): драйверів у застосунку нема.
- Гілки `feature/ui-gpio`, `feature/telemetry`, `feature/uboot` на платі не перевірялись.
