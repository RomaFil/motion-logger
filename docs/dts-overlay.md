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

## Що ще не перевірено

Образ із цим overlay ще не завантажувався на платі. Особливо потребують перевірки: чи застосовує прошивка Pi 5 наш `.dtbo`, чи збігаються імена модулів у `modules-load.d` з реальними `.ko`, і чи MPU6050 за адресою `0x68` не показується як `UU` через інший драйвер (це нормально, якщо його зайняв драйвер ядра).
