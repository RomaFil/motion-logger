# Журнал змін

Фіксуємо те, що впливає на збірку образу чи поведінку на платі (вимога менторів Ajax: суттєві зміни збірки
описувати окремо). Дрібні правки коду дивіться в `git log`.

## 09.11.2026

- **Конфігурація за правилом ментора про DTS: дубль критичного, коментарі «навіщо».** `linux.fragment` тепер
  містить 12 рядків (було 2): драйвери MPU6050 і ADXL345, I2C/SPI Designware, `spidev`, `i2c-dev`, Wi-Fi
  (`cfg80211`, `brcmfmac`). Значення збігаються зі стоковим `bcm2712_defconfig`, тож образ за змістом той самий;
  новий `board/motion_logger/tests/check_kernel_fragment.sh` звіряє фрагмент із `.config` збірки (на зіпсованій копії
  падає). Виправлено коментар у `config.txt` про `dtparam` (дубль навмисний), пояснено три базові DTB у defconfig,
  `cmdline.txt` у README, відсутність `interrupts` у MPU6050. Поведінка на платі не змінюється.
  **Образ `52a63cb`** (SHA-256 `ccf4db871d989bfdc8b4fbd031f54020fad3c91585b61ac44c55966f69f6a6c1`) зібрано на VPS і
  перевірено без плати проти `3ed1ef6`: розмітка, ядро (`.config`, `Image`, модулі), DTB і overlay, застосунок
  збігаються побайтово; відрізняються лише коментарі в `config.txt`, `S99motion_logger` і `etc/default/dropbear`,
  рядок дати збірки в банері U-Boot (`Nov 09 2026 - 20:19:03`, 7 байтів у `u-boot.bin`), мітка часу заголовка
  `boot.scr` (сам скрипт ідентичний) і сіль пароля root (хеш відповідає паролю з defconfig). `fdtoverlay` накладає
  наш overlay на `bcm2712d0-rpi-5-b.dtb`. **На платі не перевірено.**

## 05.11.2026

- **Кнопка: відпускання більше не перемикає паузу (`core/button_filter`).** На платі з образом `2476d6a` кнопку SW1
  тримали й відпускали: усі світлодіоди загорялись (пауза) і гасли (пауза знімалась) вже при відпусканні. Драйвер
  просив лише спад сигналу й приймав кожен спад пізніше за 200 мс після попереднього натискання, зокрема
  дрижання контакту при відпусканні (у `gpiomon` видно пари `FALLING`/`RISING` через 85 мкс). Тепер драйвер слухає
  обидва фронти, після події чекає 30 мс, зчитує справжній рівень лінії, і лише перехід «відпущено → натиснуто»
  вважається натисканням. Тест `test_button_filter` (у `make test` і `make test-tsan`); драйвер перевірено
  компіляцією проти заглушки `gpiod.h`. **Перевірено на платі з образом `3ed1ef6`:** кнопку тримали й відпускали,
  подвійного перемикання паузи нема.
- **U-Boot: менше шуму на консолі (`uboot.fragment`).** Вимкнено збереження середовища в FAT (`Unable to read
  "uboot.env"`), спробу EFI boot manager (`Cannot persist EFI variables`, `Boot failed (err=-14)`) і `preboot` з
  `pci enum; usb start;` (`No USB controllers found`). Конфігурація перевірена сухим прогоном Kconfig на `.config`
  U-Boot 2026.07. **Перевірено на платі з образом `3ed1ef6`:** банер U-Boot без цих повідомлень.
  `Net: No ethernet found.` лишається: Ethernet Pi 5 в U-Boot не підтримується.
- **Консоль: `getty` на `ttyAMA10`, U-Boot отримує консоль (`uboot-pl011-axi.patch`).** Лог з кабелем JST-SH показав
  безкінечне `can't open /dev/ttyAMA0` від `init`: UART0 (`serial@30000`, GPIO14/15) у device tree має
  `status = "disabled"`, пристрою `/dev/ttyAMA0` нема, а `BR2_TARGET_GENERIC_GETTY_PORT` вказував на нього.
  Тепер `ttyAMA10` (JST Pi 5): ядро, `getty` і спам-повідомлення зникають в одному місці. Банера U-Boot у тому ж лозі не
  було: debug-UART має `compatible = "arm,pl011-axi"`, драйвер PL011 в U-Boot його не знав. Патч додає цю сумісність і
  пропускає налаштування лінії (її вже налаштувала прошивка). Патч накладається (`patch --dry-run` на U-Boot 2026.07);
  **Перевірено на платі з образом `2476d6a` (лог UART): банер `U-Boot 2026.07`, `motion-logger login:` на `ttyAMA10`,
  спаму `can't open /dev/ttyAMA0` нема.**
- **Кнопки: автозапуск передає `--ui-button-active-low`** (`/etc/default/motion_logger`). Після складання плати в
  корпус кнопка SW1 не реагувала. Мультиметр показав, що SW1 і SW2 замикають лінію на землю; `gpiomon
  --bias=pull-up gpiochip0 13` і `... 17` (SW1 після `S99motion_logger stop`) дали `FALLING EDGE` на натискання й
  `RISING EDGE` на відпускання. Програма без прапорця чекала натискання як 1 з підтяжкою вниз. **Перевірено на платі
  з образом `3ed1ef6`:** кнопка ставить запис на паузу й знімає з неї. Запис 02.11.2026 про активний високий рівень
  стосувався попереднього підключення кнопки: під час складання в корпус підключення змінилось.
- **Образ + застосунок (гілка `claude/zealous-noether-yfvwss`: злиття `feature/uboot-wifi` і `feature/telemetry`;
  перевірено на платі з образом `3ed1ef6`, результати в кінці пункту):**
  - **Розмітка картки змінилась:** третій розділ `data` (ext4, 1 ГБ, `genimage.cfg.in`), `S12mounts` монтує його в
    `/data` і `/boot` (на запис із `sync`, як `S41wifi`). Картку треба записати заново. Лог автозапуску тепер
    `/data/motion.jsonl` з ротацією `--log-max-mb 50 --log-keep 10` (раніше `/var/log`, tmpfs).
  - `S99motion_logger` запускає програму під наглядачем: після аварії перезапуск через 2…60 с; `@ім'я` датчиків і
    частота MPU6050 як раніше.
  - Watchdog (`BR2_PACKAGE_BUSYBOX_WATCHDOG`, `S15watchdog`), NTP (`busybox.fragment` з `CONFIG_NTPD`, `S49ntp`), ім'я
    хоста `motion-logger`, `authorized_keys` з розділу boot вимикає вхід за паролем (`/etc/default/dropbear`).
  - Застосунок: збій шини датчика пропускає відлік (`read errors`), зупинка лише після 500 збоїв підряд; потік GPS не
    завершується від `read() == 0` на послідовному порту; `fdatasync` раз на 5 с; лог `0600`; рядок
    `"src":"log","event":"open"` з `boot_id`; `--iio mpu6050` за іменем.
  - Злиття `feature/telemetry`: GPS-поля `utc`, `alt_m`, `course_deg`, `hdop`, події `FREE_FALL`/`IMPACT`, гіроскоп і
    температура (`--iio-gyro`, в автозапуску `--iio-gyro @mpu6050`). Виправлено `MAX_TASKS` 4 → 5: кожна гілка мала 4
    задачі, а разом з кнопкою, ADXL345, гіроскопом і GPS їх 5 (масив задач переповнився б).
  **Перевірено на платі з образом `3ed1ef6` (ядро `6.12.61-v8-16k`):**
  - `/data` змонтовано як ext4 (`rw,nosuid,nodev,noexec`), `/boot` як vfat;
  - `S99motion_logger status`: `running (pid 288, supervisor 284)`; працюють `watchdog -t 5 /dev/watchdog`, `ntpd`,
    `crond`, `dropbear`, `motion_logger`;
  - наглядач: після `kill -9` програми `status` за 1 с показує `restarting`, за 5 с `running (pid 550)`;
  - час: на Pi 17:24:06, на ПК 17:24:03 UTC (`ntpd` синхронізував годинник);
  - `/dev/ttyAMA2` (GPS) і `/dev/ttyAMA10` (консоль) є, `/dev/ttyAMA0` нема (див. пункт про консоль);
  - лог: рядки `open`, `gyro` з `temp_c`, `accel`, `accel2`, `gps`; ротація за 50 МБ, події `IMPACT` і `FREE_FALL`
    з'являються.
  Не перевірено: GPS з фіксом (у приміщенні лише `fix:false`), пороги `FREE_FALL`/`IMPACT` на справжньому записі.
- **Живлення (знахідка під час перевірки):** під навантаженням усіх 4 ядер на 100 % Pi вимикається з блоком живлення
  15 Вт. Питання закрито.

## 01.11.2026

- **Застосунок + образ (гілка `feature/wifi-ui`; кнопка, пауза і етапи 1 і 2 перевірені на платі 02.11.2026, у образ не збиралось):** об'єднано крок 5.4 (кнопка,
  пауза запису, `feature/ui-gpio`) із світлодіодами етапів руху. Опція `--ui-gpio <чіп>` (замість `--stage-leds`):
  кнопка GPIO17 ставить запис на паузу й знімає з неї, світлодіод 1 (GPIO22) до руху, 2 (GPIO6) під час руху, 3 (GPIO27)
  після зупинки, на паузі горять всі три; після паузи запис і фільтр руху починаються заново. `--ui-no-button` лише
  світлодіоди, `--ui-button-active-low` кнопка дає 0 при натисканні. Автозапуск стартує з кнопкою (`--ui-gpio gpiochip0`).
- **Образ m6 (крок 5.6, збірка з коміту `c9d178a`, тег `m6`; перевірено на платі з чистої картки 02.11.2026):**
  `sdcard-2026-11-01-m6-feature-c9d178a.img`, SHA-256
  `d562e15a671e6b751b868f95771f6ab7a3fe4d7939d28ca4094ebee329c47c18`. Після вмикання без жодних дій плата за ~40 с
  в мережі Wi-Fi, `motion_logger` стартує сам (init, PID 280, через 13,6 с після старту ядра), датчики знайдено за
  ім'ям, `mpu6050` на 100 Гц, лог росте (accel, accel2, gps), `S99motion_logger status` повертає 0. Мережі Wi-Fi
  беруться з `wifi.secret` на машині збірки, у git їх нема.
- **Образ (крок 5.5, перевірено на платі зі старим образом і підміненими файлами: після вимкнення-ввімкнення
  живлення застосунок стартує сам за ~16 с, лог росте):** `/etc/default/motion_logger` тепер має робочі параметри
  (`--iio @mpu6050 --iio-rate 100 --iio-aux @adxl345 --gps /dev/ttyAMA2 --log /var/log/motion.jsonl
  --stage-leds gpiochip0`); `S99motion_logger` замінює `@ім'я` на каталог `iio:deviceN` за іменем датчика
  і виставляє `sampling_frequency` MPU6050 на 100 Гц (`MOTION_LOGGER_IIO_RATES`). Лог лежить на tmpfs.
  `config.txt`: `gpio=6=op,dl` (середній світлодіод не тліє від підтяжки).
- **Застосунок:** фільтр руху з гістерезисом: поріг входу 0,03 g, поріг виходу 0,015 g (було один поріг 0,02 g).
  На записі «стою, іду, стою» (22:47) зникла хибна пара подій біля 50-ї секунди, ходьба 20–40 с знайдена як і раніше;
  на записі 01.11 лишився один `MOVE START` (59,62 с замість 59,57 с). `analyze_recording` отримав ключ `-r`.
- **Застосунок:** опція `--dump-raw <файл>` пише сирі відліки основного акселерометра в CSV (формат `accel_csv`),
  щоб звірити події справжнього запису з `analyze_recording` (крок 5.3 на платі).
- **Застосунок + образ (не збирався, на платі не перевірявся):** опція `--stage-leds <чіп>` показує етап руху трьома
  світлодіодами (до руху, рух, після зупинки; GPIO22, GPIO6, GPIO27). Пакет `motion_logger` збирається з `GPIOD=1`
  і залежить від `libgpiod` (вона вже в `defconfig`, нових пакетів немає).
- **Образ (не збирався, на платі не перевірявся; гілка `feature/wifi`):** Wi-Fi замість Ethernet. У `defconfig`:
  `linux-firmware` (`BRCM_BCM43XXX`), `wireless-regdb`, `wpa_supplicant` (nl80211, `wpa_cli`). Новий скрипт
  `S41wifi` читає мережі з `/boot/wifi.txt` (FAT-розділ картки, у git його немає), генерує конфіг у `/run` і
  запускає `wpa_supplicant` та `udhcpc` на `wlan0`; `brcmfmac` додано в `modules-load.d`; NVRAM Pi 5 це симлінк
  на файл Pi 4. `post-image.sh` кладе `~/workspace/wifi.secret` у boot-розділ образу. Подробиці: `docs/wifi.md`.
  Після першої збірки: `wlan0` має адресу, SSH по Wi-Fi працює без кабелю.
  `S41wifi` також пише знімки стану Wi-Fi у `/boot/wifi-debug.log` (читається з Windows без консолі).
  Далі: країна більше не задається за замовчуванням (у логу з `country=UA` регдомен змінювався постійно), `brcmfmac roamoff=1 feature_disable=0x82000` (без останнього wpa_supplicant 2.12 не дочікувався `port authorized` від brcmfmac, `Authentication timed out`, цикл розривів). У `defconfig` пакет `iw`, `S41wifi` вимикає `power_save` на `wlan0` (при увімкненому плата мала оренду DHCP, але не відповідала з ПК).

## 29.10.2026 (гілка feature/ui-gpio, ще не злита в main)

- **Застосунок:** кнопка і RGB-світлодіод (`--ui-gpio`, крок 5.4): пауза запису, колір за станом; `log_writer_set_paused()` і `log_writer_is_moving()`; драйвер `drivers/gpiod_ui` на libgpiod 1.x, збирається з `GPIOD=1`.
- **Образ:** пакет `motion_logger` залежить від `libgpiod` і збирається з `GPIOD=1`. Не збиралось у Buildroot.

## 29.10.2026 (гілка feature/telemetry, ще не злита в main)

- **Застосунок:** GPS додає в лог час UTC, висоту, курс і HDOP (`core/utc_time`, розбір RMC/GGA); програмні події `FREE_FALL` і `IMPACT` (`core/impact_detector`); гіроскоп і температура MPU6050 (`--iio-gyro`, рядок `gyro` раз на секунду). Старі поля рядків не змінились, нові йдуть після них.

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
