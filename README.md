# Motion Logger

Фінальний проєкт стажування Ajax Embedded Linux Internship: пристрій на **Raspberry Pi 5** збирає дані
акселерометрів (MPU6050 по I2C, ADXL345 по SPI) і GPS (NEO-6M по UART), визначає рух і пише лог.
Система збирається **Buildroot** у власний образ `sdcard.img`.

Цей репозиторій є **зовнішнім деревом Buildroot** (`BR2_EXTERNAL`): наш код і конфіги лежать окремо від Buildroot.

## Структура

| Шлях | Що там |
|---|---|
| `app/` | код застосунку і його `Makefile`: `core/` (буфер, фільтр, лог; без заліза), `interfaces/` (`sensor.h`: «дай відлік»), `drivers/` (звідки беруться відліки: поки програвач CSV), `main.c` (єдине місце, що з'єднує ядро з драйвером), `tests/` |
| `package/motion_logger/` | рецепт Buildroot для застосунку |
| `configs/` | defconfig образу: `motion_logger_defconfig` (`savedefconfig` від `raspberrypi5_defconfig` + наші зміни) |
| `board/motion_logger/` | файли плати: `rootfs_overlay/` (скрипт автозапуску), фрагменти ядра й BusyBox, DTS |
| `docs/` | документація, boot-логи (`docs/bootlogs/`) |
| `external.desc`, `external.mk`, `Config.in` | опис external-дерева (ім'я `MOTION_LOGGER`) |

## Збірка застосунку окремо (для розробки)

    cd app
    make                                   # на цьому комп'ютері
    make CROSS_COMPILE=aarch64-linux-gnu-  # для Raspberry Pi 5
    make STATIC=1                          # статична збірка (не залежить від libc на цілі)
    make DEBUG=1                           # з відладочною інформацією (-g -O0)
    make install DESTDIR=/tmp/rootfs       # встановити motion_logger в /tmp/rootfs/usr/bin
    make -n                                # лише показати команди, нічого не виконуючи
    make SANITIZE=address,undefined test   # усі тести з перевіркою пам'яті (make clean до і після)
    make analyze                           # інструмент аналізу запису акселерометра й GPS
    make check-samples                     # звіт на tests/data збігається з еталоном і з ground_truth
    make check-replay                      # програти tests/data через motion_logger --replay, події в лозі збігаються зі звітом
    make test                              # тести буфера і читача (лише на цьому комп'ютері)
    make test-tsan                         # те саме з перевіркою гонок даних
    make format-check CLANG_FORMAT=~/.venvs/clang23/bin/clang-format   # перевірка стилю
    make help

## Збірка образу

Потрібен Buildroot **2026.08** (окремий каталог, лише upstream):

    git clone --branch 2026.08 https://gitlab.com/buildroot.org/buildroot.git
    cd buildroot
    make O=../build-pi5 BR2_EXTERNAL=/шлях/до/цього/репозиторію motion_logger_defconfig
    cd ../build-pi5 && make

Образ: `build-pi5/images/sdcard.img`. Запис на картку: `dd if=sdcard.img of=/dev/sdX bs=4M conv=fsync`
(перевірте `X`: помилка зітре чужий диск). Повну збірку з нуля чекайте близько 1,5 години; після зміни лише
застосунку досить `make motion_logger-rebuild && make` (див. `CHANGELOG.md`).

## Схема підключення (плата Motion Logger на Raspberry Pi 5)

Нумерація пінів фізична (гребінка 40-pin); живлення модулів 3,3 В з пінів 1 і 17. Детальна схема й PCB
лежать в проєкті Altium (поза цим репозиторієм).

| Модуль | Шина | Пін Pi (GPIO) | Пін модуля |
|---|---|---|---|
| GY-87 / HW-290 (MPU6050 `0x68`, HMC5883L `0x1E`, BMP180 `0x77`) | I2C1 | 3 (GPIO2 SDA), 5 (GPIO3 SCL) | SDA, SCL |
| GY-291 (ADXL345) | SPI0, CE0 | 19 (GPIO10 MOSI), 21 (GPIO9 MISO), 23 (GPIO11 SCLK), 24 (GPIO8 CE0) | SDA, SDO, SCL, CS |
| NEO-6M (HW-623) | UART2 | 7 (GPIO4 TX), 29 (GPIO5 RX) | RXD, TXD (навхрест) |
| Кнопки | GPIO | 11 (GPIO17), 33 (GPIO13) | S |
| Світлодіоди R / G / B | GPIO | 13 (GPIO27), 15 (GPIO22), 31 (GPIO6) | через резистор 220 / 100 / 100 Ом |
| Налагоджувальний UART `DBG1` | UART0 | 8 (GPIO14 TX), 10 (GPIO15 RX) | USB-UART 3,3 В |

## Підключення до плати і перевірка

- Консоль: USB-UART 3,3 В на `DBG1` (115200 8N1; `picocom -b 115200 /dev/ttyUSB0`), TX адаптера на RX плати і навпаки.
  Повідомлення ядра йдуть на роз'єм JST (`ttyAMA10`), на `DBG1` лише getty та вивід `S99motion_logger`.
- Мережа: DHCP на `eth0`, SSH-сервер dropbear (користувач `root`, навчальний пароль задано в `configs/motion_logger_defconfig`;
  у продуктовому образі його прибрати).
- Автозапуск: `/etc/init.d/S99motion_logger {start|stop|restart|status}`, параметри в `/etc/default/motion_logger`.
- Перевірка заліза в образі (потрібні `i2c_arm` і `spi` у `config.txt` та модулі з `/etc/modules-load.d/`):

      i2cdetect -y 1        # очікується 68, 1e (після bypass), 77
      ls /dev/spidev0.0     # ADXL345 (DEVID 0xE5, SPI mode 3)
      gpiodetect; gpioinfo  # лінії кнопок і світлодіодів (libgpiod 1.6.5: gpioset gpiochip0 27=1)

  Це список для **навчального** образу; у продуктовому `i2c-tools` і утиліти libgpiod прибирають.
- Перевірка init-скрипта без плати: `sh board/motion_logger/tests/check_init_script.sh`.

## Імітатор датчика

    cd app && make
    ./build/sensor_sim | ./build/motion_logger --stdin --log motion.jsonl   # 100 чисел на секунду; Ctrl+C друкує підсумок

`sensor_sim` пише числа в канал раз на 10 мс, `motion_logger --stdin` читає їх через epoll у кільцевий буфер. Без `--stdin` програма нічого не читає й виходить (так її запускає автозапуск образу). З `--log` окремий потік дописує кожне число рядком JSON у файл: `{"t_ms":396411,"src":"sim","value":299}` (`t_ms` це монотонний годинник у мілісекундах, мітка ставиться в момент читання числа).

## Програвач запису акселерометра (замість справжнього датчика)

    cd app && make
    ./build/motion_logger --replay tests/data/accel_sample.csv --speed 50 --log motion.jsonl

Драйвер `drivers/csv_player.c` віддає відліки з CSV (`t_ms,x_g,y_g,z_g`) через інтерфейс `struct sensor` (`interfaces/sensor.h`); ядро не знає, що це файл, а не плата. `--speed 1` (за замовчуванням) це реальний час, `50` у 50 разів швидше, `0` без пауз (буфер може переповнитись, втрати видно в підсумку). У лог іде не кожен відлік, а лише зміна стану руху: 

    {"t_ms":20550,"src":"accel","event":"MOVE_START"}
    {"t_ms":40550,"src":"accel","event":"MOVE_STOP"}

Рух рахують ті самі модулі й ті самі параметри (`core/motion_defaults.h`), що й в `analyze_recording`, тому події в лозі збігаються зі звітом інструмента (це перевіряє `make check-replay`). Параметри (вікно 10, поріг 0,05 g, підтвердження 50 відліків) поки припущення.

## Аналіз запису акселерометра і GPS

    cd app && make analyze
    ./build/analyze_recording [-w вікно] [-t поріг_g] [-c підтвердження] tests/data/accel_sample.csv tests/data/gps_sample.nmea

Друкує по секундах «рух так/ні» і середнє відхилення (g), події `MOVE START` / `MOVE STOP` з часом
та підсумок по рядках NMEA. Дані в `app/tests/data/` зараз **синтетичні** (вигадані, див.
`app/tests/data/README.md`): справжній запис з плати кладеться на їхнє місце з тим самим форматом,
після чого поріг і вікно підбираються цим же інструментом.

## Статус

Каркас (крок 2.1) і власний образ, що завантажується на Pi 5 (консоль на GPIO14, getty, dropbear). Готові: кільцевий буфер з замком (крок 2.2–2.3), читач на epoll з імітатором (крок 2.4), запис у лог (крок 2.5), одна команда `make test` і прапорці Makefile (крок 2.6), розбір NMEA, фільтр руху, події руху й аналіз запису (кроки 3.2–3.5 на синтетичних даних), поділ на `core/` / `interfaces/` / `drivers/` з програвачем CSV і події `MOVE_START`/`MOVE_STOP` у лозі (кроки 5.1 і 5.3 на синтетичних даних). Далі: читання справжніх датчиків.
