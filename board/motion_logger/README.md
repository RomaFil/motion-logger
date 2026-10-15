# board/motion_logger

Файли, специфічні для нашої плати:

- `rootfs_overlay/` копіюється в корінь образу (скрипт автозапуску `etc/init.d/S99motion_logger`);
- `linux.fragment`, `busybox.fragment` (кроки 4.5, 4.6): наші зміни до налаштувань ядра й BusyBox;
- `dts/`: DTS-overlay для датчиків (крок 4.6).

## Образ: що додано до стокового raspberrypi5_defconfig
- `config.txt`, `cmdline.txt`: `enable_uart=1`, `enable_rp1_uart=1` (лог прошивки й `getty` на GPIO14, `ttyAMA0`).
- `post-image.sh`, `genimage.cfg.in`: копія стокових із Buildroot 2026.08 + `bcm2712d0.dtbo` з каталогу ядра (Pi 5 з чипом D0, наприклад 16 ГБ; без нього прошивка пише `bcm2712d0.dtbo not found`).
- Консоль входу: `BR2_TARGET_GENERIC_GETTY_PORT="ttyAMA0"`, 115200; повідомлення ядра на GPIO14 не видно (драйвер `pl011-axi`), вони йдуть на JST-роз'єм (`ttyAMA10`).
- `dropbear` (SSH по Ethernet), демонстраційний пароль root у defconfig (ніде більше не використовується).
