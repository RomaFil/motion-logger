# Журнал змін

Фіксуємо те, що впливає на збірку образу чи поведінку на платі (вимога менторів Ajax: суттєві зміни збірки
описувати окремо). Дрібні правки коду дивіться в `git log`.

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
