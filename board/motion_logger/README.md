# board/motion_logger

Файли, специфічні для нашої плати:

- `rootfs_overlay/` копіюється в корінь образу (скрипт автозапуску `etc/init.d/S99motion_logger`);
- `linux.fragment`, `busybox.fragment` (кроки 4.5, 4.6): наші зміни до налаштувань ядра й BusyBox;
- `dts/`: DTS-overlay для датчиків (крок 4.6).
