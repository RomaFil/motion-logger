# U-Boot між прошивкою Pi 5 і ядром (крок 4.7)

> Гілка `feature/uboot`. На платі **не перевірено**: корпус і консольний кабель JST-SH ще чекаємо. Основна гілка `main` збирає образ без U-Boot (`kernel=Image`).

## Що змінюється в ланцюгу завантаження

Без U-Boot (гілка `main`): прошивка плати → ядро `Image` → `init`.
З U-Boot (ця гілка): прошивка плати → `u-boot.bin` → `boot.scr` → ядро `Image` → `init`.

Прошивка Pi 5 (GPU-прошивка і EEPROM) лишається першою ланкою: вона застосовує `dtoverlay` з `config.txt` до device tree і передає цей уже готовий опис у U-Boot. Власного дерева для Pi 5 в U-Boot немає (станом на червень 2026), тому U-Boot користується тим, що дала прошивка. Датчики описані overlay для Linux (`docs/dts-overlay.md`), U-Boot про них не знає: в U-Boot лише мінімум обладнання, потрібний щоб завантажити ядро (порада ментора Юрія, 29.10.2026).

## Що додано в цій гілці

| Файл | Що робить |
|---|---|
| `configs/motion_logger_defconfig` | `BR2_TARGET_UBOOT=y`, плата `rpi_arm64`, `uboot.fragment` вимикає `CONFIG_TOOLS_MKEFICAPSULE` (інакше потрібен gnutls) |
| `board/motion_logger/config.txt` | `arm_64bit=1`, `kernel=u-boot.bin` замість `kernel=Image` |
| `board/motion_logger/boot.cmd` | скрипт U-Boot: задає `bootargs`, вантажить `Image` з розділу boot, `booti` |
| `board/motion_logger/post-image.sh` | коли `kernel=u-boot.bin`: додає `Image` на розділ boot і збирає `boot.scr` з `boot.cmd` програмою `mkimage` з дерева збірки U-Boot |

Рядок ядра в `boot.cmd` (`root=/dev/mmcblk0p2 rootwait console=tty1 console=ttyAMA10,115200`) дублює `cmdline.txt`, бо U-Boot бере `bootargs` зі свого середовища. Змінюєш один файл, змінюй і другий.

## Як перевірити на платі

1. Зібрати образ (`scripts/build.sh` або `make` у каталозі збірки), записати `sdcard.img` на **окрему** картку.
2. Підключити JST-кабель до відлагоджувального UART (115200), `picocom -b 115200 /dev/ttyUSB0`.
3. Увімкнути плату. Очікуємо: рядки U-Boot з версією, потім виконання `boot.scr`, `Starting kernel ...`, повідомлення ядра, вхід `getty`.
4. Натиснути будь-яку клавішу під час затримки автозавантаження: має з'явитись командний рядок `U-Boot>`. Корисні команди: `printenv`, `mmc list`, `ls mmc 0:1`, `fdt addr ${fdt_addr}`, `fdt print /chosen`.
5. Після входу в систему перевірити, що датчики працюють як без U-Boot: `ls /sys/bus/iio/devices/`.

## Якщо не завантажується

- Тиша на UART: прошивка не знайшла `u-boot.bin` (перевір розділ boot: `u-boot.bin`, `config.txt`) або кабель підключено не до того UART.
- Банер U-Boot є, ядро не стартує: перевір `ls mmc 0:1` (чи є `Image` і `boot.scr`), змінні `kernel_addr_r`, `fdt_addr`; руками: `load mmc 0:1 ${kernel_addr_r} Image` і `booti ${kernel_addr_r} - ${fdt_addr}`.
- `Starting kernel ...` і далі panic: рядок `bootargs`, device tree, `root=`.
- Повернення до базового образу: у `config.txt` `kernel=Image` (або картка зі збіркою з `main`).

## Відомі невизначеності

- Номер пристрою `mmc 0` для SD на Pi 5 в U-Boot; у `boot.cmd` ми користуємось змінними `devtype`, `devnum`, `distro_bootpart`, які виставляє distro boot, щоб не зашивати номер.
- Чи залишається в device tree від прошивки застосований `dtoverlay=motion-logger-sensors` (за описом прошивки так, перевіряється командою `fdt print /soc/...` або просто появою датчиків в IIO).
- Чи достатньо `rpi_arm64_defconfig` для SD на Pi 5: U-Boot 2026.07 з Buildroot 2026.08 **зібрано на VPS** (`u-boot.bin` ≈ 0,9 МБ), `mkimage` зібрав `boot.scr` з `boot.cmd` без помилок; повний образ із U-Boot не збирався, на платі не запускалось. Мережа, USB і NVMe в U-Boot для Pi 5 неповні, для завантаження з SD це не заважає.
