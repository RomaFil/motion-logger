# board/motion_logger

Файли, специфічні для нашої плати:

- `rootfs_overlay/` копіюється в корінь образу (скрипт автозапуску `etc/init.d/S99motion_logger`);
- `linux.fragment`, `busybox.fragment` (кроки 4.5, 4.6): наші зміни до налаштувань ядра й BusyBox (у BusyBox: `ntpd`);
- `dts/`: DTS-overlay для датчиків (крок 4.6).

## Образ: що додано до стокового raspberrypi5_defconfig
- `config.txt`, `cmdline.txt`: `enable_uart=1`, `enable_rp1_uart=1` (лог прошивки на GPIO14). UART0 (GPIO14/15, `DBG1`) у device tree
  вимкнений (`status = "disabled"`), тому `/dev/ttyAMA0` у цьому образі нема.
- `cmdline.txt`: `console=tty1 console=ttyAMA10,115200`. Коментарів у цьому файлі писати не можна, тому пояснення тут:
  `tty1` лишено від стокового образу (HDMI-консоль, якщо під'єднано дисплей), `ttyAMA10` це debug UART на JST-роз'ємі
  (повідомлення ядра), узгоджений із `BR2_TARGET_GENERIC_GETTY_PORT`.
- Правило для DTS і конфігурації ядра (Юрій Безкор, 09.11.2026): лишаємо стокове, що не заважає; критичне дублюємо
  локально (`linux.fragment`, `dtparam` у `config.txt`), щоб при зміні версії Buildroot чи платформи було видно
  мінімально необхідну конфігурацію; кожну таку зміну коментуємо «навіщо». Звірка `linux.fragment` з `.config` збірки:
  `sh board/motion_logger/tests/check_kernel_fragment.sh <шлях до build/linux-custom/.config>`.
- `post-image.sh`, `genimage.cfg.in`: копія стокових із Buildroot 2026.08 + `bcm2712d0.dtbo` з каталогу ядра (Pi 5 з чипом D0, наприклад 16 ГБ; без нього прошивка пише `bcm2712d0.dtbo not found`).
- Консоль входу: `BR2_TARGET_GENERIC_GETTY_PORT="ttyAMA10"`, 115200: повідомлення ядра, вхід і (з патчем `uboot-pl011-axi.patch`) банер
  U-Boot йдуть на один JST-роз'єм. З `ttyAMA0` `init` без кінця друкував `can't open /dev/ttyAMA0`.
- Служби `rootfs_overlay/etc/init.d`: `S12mounts` (`/boot` на запис із `sync`, `/data` з третього розділу), `S41wifi`,
  `S49ntp` (BusyBox `ntpd`), `S99motion_logger` (з наглядачем, що перезапускає програму); `S15watchdog` ставить BusyBox.
- `genimage.cfg.in`: розділи boot, rootfs і `data` (ext4, 1 ГБ, мітка `data`).
- `rootfs_overlay/etc/default/dropbear`: ключ з `/boot/authorized_keys` і вхід лише за ключем.
- Тести скриптів: `tests/check_init_script.sh`, `tests/check_overlay.sh`.
- `dropbear` (SSH по Ethernet), демонстраційний пароль root у defconfig (ніде більше не використовується).
