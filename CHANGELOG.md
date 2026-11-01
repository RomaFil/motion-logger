# Журнал змін

Фіксуємо те, що впливає на збірку образу чи поведінку на платі (вимога менторів Ajax: суттєві зміни збірки
описувати окремо). Дрібні правки коду дивіться в `git log`.

## 01.11.2026

- **Образ (не збирався, на платі не перевірявся; гілка `feature/wifi`):** Wi-Fi замість Ethernet. У `defconfig`:
  `linux-firmware` (`BRCM_BCM43XXX`), `wireless-regdb`, `wpa_supplicant` (nl80211, `wpa_cli`). Новий скрипт
  `S41wifi` читає мережі з `/boot/wifi.txt` (FAT-розділ картки, у git його немає), генерує конфіг у `/run` і
  запускає `wpa_supplicant` та `udhcpc` на `wlan0`; `brcmfmac` додано в `modules-load.d`; NVRAM Pi 5 це симлінк
  на файл Pi 4. `post-image.sh` кладе `~/workspace/wifi.secret` у boot-розділ образу. Подробиці: `docs/wifi.md`.
  Після першої збірки: `wlan0` має адресу, SSH по Wi-Fi працює без кабелю.
  `S41wifi` також пише знімки стану Wi-Fi у `/boot/wifi-debug.log` (читається з Windows без консолі).

## 29.10.2026

- **Образ (не збирався, на платі не перевірявся):** overlay `motion-logger-sensors.dtbo` (MPU6050 на `i2c1`
  за адресою 0x68, ADXL345 на `spi0` CS0 у режимі 3, `spidev0` вимкнено, щоб драйвер ядра не конфліктував) збирається
  в `post-image.sh` компілятором `dtc` з ядра; `config.txt` підключає його та `uart2-pi5`; фрагмент ядра
  `linux.fragment` вмикає `CONFIG_ADXL345_SPI=m`; у `modules-load.d` додано `inv-mpu6050-i2c` і `adxl345_spi`.
  Після першої збірки: `ls /sys/bus/iio/devices/` має показати два пристрої, `cat .../name` дає `mpu6050` і `adxl345`.
- **Застосунок:** нові джерела `--iio`, `--iio-aux`, `--gps` (драйвер `drivers/iio_accel`, `drivers/uart_port`,
  `core/gps_reader`), кожне у власному потоці; у лог додано рядки `accel` з підсумком секунди, `accel2`, `gps`.
  Формат рядків у README. Старі рядки подій руху не змінилися.
- **Скрипти:** `scripts/record_accel.py` записує акселерометр і GPS для підбору порога руху (README у `app/tests/data/`).

## 28.10.2026

- **Образ (не збирався, на платі не перевірявся):** `config.txt` вмикає `i2c_arm` і `spi`; новий
  `rootfs_overlay/etc/modules-load.d/motion_logger.conf` вантажить `i2c-designware-platform`, `i2c-dev`,
  `spi-dw-mmio`, `spidev` (в ядрі це модулі); у `defconfig` додано `i2c-tools`, `libgpiod` (1.6.5 у Buildroot 2026.08)
  і її утиліти. Після першої збірки перевірити `i2cdetect -y 1` і `ls /dev/spidev0.0`.
- **Автозапуск:** `S99motion_logger` став ідемпотентним (повторний `start` повертає 0), `stop` чекає завершення і
  добиває `SIGKILL`, додано `status`; параметри запуску в `/etc/default/motion_logger`.
- **Застосунок:** запис у буфері став `struct sample`; `interfaces/sensor.h`, драйвер `drivers/csv_player`
  (програвач CSV-запису), події `MOVE_START` / `MOVE_STOP` у лозі; режим `motion_logger --replay`.
- **Документація:** у README схема підключення модулів, підключення до плати і перевірка.

## 15.10.2026

- Власний образ завантажується на Raspberry Pi 5 (консоль на `DBG1`, getty, dropbear).
- Кільцевий буфер із замком (`app/core/ring_buffer`).

## 13.10.2026

- Каркас `BR2_EXTERNAL` (`MOTION_LOGGER`), `motion_logger_defconfig`, перша збірка Buildroot 2026.08 на VPS.
