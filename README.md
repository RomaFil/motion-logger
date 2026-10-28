# Motion Logger

Фінальний проєкт стажування Ajax Embedded Linux Internship: пристрій на **Raspberry Pi 5** збирає дані
акселерометрів (MPU6050 по I2C, ADXL345 по SPI) і GPS (NEO-6M по UART), визначає рух і пише лог.
Система збирається **Buildroot** у власний образ `sdcard.img`.

Цей репозиторій є **зовнішнім деревом Buildroot** (`BR2_EXTERNAL`): наш код і конфіги лежать окремо від Buildroot.

## Структура

| Шлях | Що там |
|---|---|
| `app/` | код застосунку (`core/`, `drivers/`, `interfaces/`, `tests/`) і його `Makefile` |
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

Образ: `build-pi5/images/sdcard.img`.

## Імітатор датчика

    cd app && make
    ./build/sensor_sim | ./build/motion_logger --stdin --log motion.jsonl   # 100 чисел на секунду; Ctrl+C друкує підсумок

`sensor_sim` пише числа в канал раз на 10 мс, `motion_logger --stdin` читає їх через epoll у кільцевий буфер. Без `--stdin` програма нічого не читає й виходить (так її запускає автозапуск образу). З `--log` окремий потік дописує кожне число рядком JSON у файл: `{"t_ms":396411,"src":"sim","value":299}` (`t_ms` це монотонний годинник у мілісекундах).

## Статус

Каркас (крок 2.1) і власний образ, що завантажується на Pi 5 (консоль на GPIO14, getty, dropbear). Готові: кільцевий буфер з замком (крок 2.2–2.3), читач на epoll з імітатором (крок 2.4), запис у лог (крок 2.5), одна команда `make test` і прапорці Makefile (крок 2.6). Далі: фільтр руху та читання датчиків.
