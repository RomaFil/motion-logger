# board/motion_logger

Файли, специфічні для нашої плати:

- `rootfs_overlay/` копіюється в корінь образу (скрипт автозапуску `etc/init.d/S99motion_logger`);
- `linux.fragment`, `busybox.fragment` (кроки 4.5, 4.6): наші зміни до налаштувань ядра й BusyBox (у BusyBox: `ntpd`);
- `dts/`: DTS-overlay для датчиків (крок 4.6).

## Образ: що додано до стокового raspberrypi5_defconfig
- `config.txt`, `cmdline.txt`: `enable_uart=1`, `enable_rp1_uart=1` (лог прошивки й `getty` на GPIO14, `ttyAMA0`).
- `post-image.sh`, `genimage.cfg.in`: копія стокових із Buildroot 2026.08 + `bcm2712d0.dtbo` з каталогу ядра (Pi 5 з чипом D0, наприклад 16 ГБ; без нього прошивка пише `bcm2712d0.dtbo not found`).
- Консоль входу: `BR2_TARGET_GENERIC_GETTY_PORT="ttyAMA0"`, 115200; повідомлення ядра на GPIO14 не видно (драйвер `pl011-axi`), вони йдуть на JST-роз'єм (`ttyAMA10`).
- Служби `rootfs_overlay/etc/init.d`: `S12mounts` (`/boot` на запис із `sync`, `/data` з третього розділу), `S41wifi`,
  `S49ntp` (BusyBox `ntpd`), `S99motion_logger` (з наглядачем, що перезапускає програму); `S15watchdog` ставить BusyBox.
- `genimage.cfg.in`: розділи boot, rootfs і `data` (ext4, 1 ГБ, мітка `data`).
- `rootfs_overlay/etc/default/dropbear`: ключ з `/boot/authorized_keys` і вхід лише за ключем.
- Тести скриптів: `tests/check_init_script.sh`, `tests/check_overlay.sh`.
- `dropbear` (SSH по Ethernet), демонстраційний пароль root у defconfig (ніде більше не використовується).
