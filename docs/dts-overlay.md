# Device Tree: що змінює наш overlay

Файл `board/motion_logger/dts/motion-logger-sensors.dts` це **overlay**, тобто невелика добавка до базового опису Raspberry Pi 5, а не повна заміна. Він каже ядру три речі: на шині I2C1 за адресою `0x68` сидить акселерометр MPU6050, на шині SPI0 на лінії вибору CE0 сидить акселерометр ADXL345 (режим SPI 3, до 5 МГц), а загальний `spidev` на цій лінії вимкнено, щоб не конфліктував із драйвером ADXL345. Завдяки цьому ядро саме підвантажує драйвери `inv-mpu6050-i2c` і `adxl345_spi`, і датчики з'являються як пристрої IIO в `/sys/bus/iio/devices/`.

## Що саме в файлі

| Фрагмент | Ціль | Що змінює |
|---|---|---|
| `fragment@0` | `&i2c1` (GPIO2/3) | вмикає шину, 100 кГц, додає `imu@68` з `compatible = "invensense,mpu6050"` |
| `fragment@1` | `&spi0` (CE0 = GPIO8) | вмикає шину, додає `accelerometer@0` з `compatible = "adi,adxl345"`, `spi-max-frequency = <5000000>`, `spi-cpol`, `spi-cpha` |
| `fragment@2` | `&spidev0` | `status = "disabled"`: базове дерево вже віддає CE0 загальному `spidev`, а на одному CS не може бути двох власників |

Лінія переривання ADXL345 (INT1) на платі не підключена, тому `interrupts` не задано: драйвер працює лише з читанням сирих значень, нам цього досить (100 Гц опитування).

## Як overlay потрапляє в образ

1. `board/motion_logger/post-image.sh` компілює файл `dtc` із дерева зібраного ядра з ключем `-@` (він додає розділ `__fixups__`, за яким прошивка знаходить мітки `i2c1`, `spi0`, `spidev0` у базовому дереві). Результат: `rpi-firmware/overlays/motion-logger-sensors.dtbo`.
2. Той самий скрипт копіює `uart2-pi5.dtbo` з ядра (UART2 для GPS на `/dev/ttyAMA2`) та `bcm2712d0.dtbo`.
3. `board/motion_logger/config.txt` підключає overlay рядком `dtoverlay=motion-logger-sensors` (і `dtoverlay=uart2-pi5`); прошивка плати застосовує їх до базового дерева ще до запуску ядра.
4. Драйвери зібрано модулями (`CONFIG_ADXL345_SPI=m` додано фрагментом `board/motion_logger/linux.fragment`, старий `INPUT_ADXL34X` вимкнено, бо блокує новий), їх вантажить `S11modules` зі списку `etc/modules-load.d/motion_logger.conf`.

## Як перевірити на платі

    ls /sys/bus/iio/devices/                      # два пристрої: iio:device0 і iio:device1
    cat /sys/bus/iio/devices/iio:device*/name     # mpu6050 і adxl345 (порядок може бути будь-який)
    cat /sys/bus/iio/devices/iio:device0/in_accel_x_raw   # число, що змінюється при нахилі плати
    dmesg | grep -i -e mpu6050 -e adxl345         # драйвери підхопили вузли

## Перевірено на платі (образ `da1be05`, 02.11.2026)

| Що | Команда | Результат |
|---|---|---|
| Прошивка застосувала overlay | `find /sys/firmware/devicetree/base -name "imu@68" -o -name "accelerometer@0"` | `…/i2c@74000/imu@68` і `…/spi@50000/accelerometer@0` є в живому дереві |
| Драйвери підхопили вузли | `readlink /sys/bus/i2c/devices/1-0068/driver`, `readlink /sys/bus/spi/devices/spi0.0/driver` | `inv-mpu6050-i2c` і `adxl345_spi` |
| Модулі ядра | `lsmod` | `inv_mpu6050_i2c`, `adxl345_spi`, `i2c_dev`, `spidev`, `spi_dw_mmio`, `i2c_designware_platform` |
| Пристрої IIO | `cat /sys/bus/iio/devices/iio:device*/name` | `iio:device0` = `mpu6050`, `iio:device1` = `adxl345` |
| I2C | `i2cdetect -y 1` | `0x68` = `UU` (зайнято драйвером ядра, це норма), `0x77` (барометр) видно, `0x1e` (магнітометр) без bypass не видно |
| `spidev0` вимкнено | `ls /dev/spidev*` | є `spidev0.1`, `spidev0.0` нема: фрагмент 2 працює, CE0 належить драйверу ADXL345 |
| Частота | `cat iio:device0/sampling_frequency`, `cat iio:device1/in_accel_sampling_frequency` | `100` і `100.000000000` |

Атрибут частоти в двох датчиків називається по-різному: `sampling_frequency` (MPU6050, його виставляє
`S99motion_logger` зі `MOTION_LOGGER_IIO_RATES`) і `in_accel_sampling_frequency` (ADXL345).

Після завантаження `dmesg` показує для MPU6050 три повідомлення: `mounting matrix not found: using identity`,
`supply vdd not found` і `supply vddio not found, using dummy regulator`. Overlay не описує матрицю орієнтації датчика
та регулятори живлення, тому драйвер бере одиничну матрицю й заглушки; на роботу це не впливає (датчик віддає
відліки), але для коректних осей на іншій платі матрицю треба описати.

## Що ще не перевірено

- I2C лишається на 100 кГц: 400 кГц не пробували.
- Лінія INT1 ADXL345 не підключена, тож режим із перериваннями не перевірявся.
- Матриця орієнтації MPU6050 не задана (одинична), осі збігаються з осями чипа, а не корпусу.
- Перевірка зроблена на одній платі й одному зібраному образі; на іншому ядрі імена модулів у `modules-load.d` треба звірити з реальними `.ko`.
