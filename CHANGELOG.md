# Журнал змін

Фіксуємо те, що впливає на збірку образу чи поведінку на платі (вимога менторів Ajax: суттєві зміни збірки
описувати окремо). Дрібні правки коду дивіться в `git log`.

## 03.11.2026

- **Образ (не збирався, на платі не перевірявся): надійність і доступ.**
  - Картка має третій розділ `data` (ext4, 1 ГБ, `genimage.cfg.in`); `S12mounts` монтує його в `/data`, а boot у
    `/boot` лише для читання. **Розмітка змінилась: картку треба записати заново.**
  - `S99motion_logger` запускає програму під наглядачем: після аварії перезапуск через 2…60 с, `status` показує обидва
    PID; сирота після `kill -9` наглядача прибирається на наступному `start`.
  - Watchdog: `BR2_PACKAGE_BUSYBOX_WATCHDOG` (`S15watchdog`, `/dev/watchdog` BCM2712).
  - NTP: `busybox.fragment` вмикає `ntpd`, служба `S49ntp` (`/etc/default/ntp`).
  - SSH-ключі: `authorized_keys` на розділі boot копіюється в `/root/.ssh` і вимикає вхід за паролем
    (`/etc/default/dropbear`).
  - `/etc/default/motion_logger`: готовий рядок для всіх датчиків, LED і логу на `/data` з ротацією.
  - Тести: `check_init_script.sh` (наглядач), новий `check_overlay.sh`.
  Після збірки перевірити: `mount | grep -E '/boot|/data'`, `S99motion_logger status`, `kill -9` програми й
  повторний `status` (перезапуск), `ls /dev/watchdog`, `date` після підключення мережі.
- **Застосунок:** збій шини датчика пропускає відлік (`read errors` у підсумку), зупинка лише після 500 збоїв підряд;
  потік GPS не завершується від `read() == 0` на послідовному порту; `fdatasync` раз на 5 с; лог `0600`;
  ротація `--log-max-mb` / `--log-keep`; рядок `"src":"log","event":"open"` з `boot_id`; поле `utc` у рядках `gps`;
  `--iio mpu6050` (за іменем); `--gpio /dev/gpiochip0`: світлодіоди стану й кнопки (`"src":"button"`).
  Формат старих рядків не змінився, лише в `gps` може з'явитися поле `utc` у кінці.
- **Образ (не збирався, на платі не перевірявся): Wi-Fi.** У `defconfig` прошивка CYW43455
  (`brcmfmac-sdio-firmware-rpi-wifi`), `wireless-regdb`, `wpa_supplicant` з `wpa_cli` і `wpa_passphrase`, ім'я хоста
  `motion-logger`. `post-image.sh` кладе шаблон `board/motion_logger/wpa_supplicant.conf` на розділ boot. Нова служба
  `S41wifi` монтує boot у `/boot` лише для читання і, якщо в файлі є мережі, вантажить `brcmfmac`, запускає
  `wpa_supplicant` і `udhcpc` на `wlan0`; нова команда `wifi add|remove|list|status` править файл на платі.
  Після збірки перевірити: `ls /lib/firmware/brcm/brcmfmac43455*`, `wifi add <ssid>`, `wifi status`, `ip addr show wlan0`.

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
