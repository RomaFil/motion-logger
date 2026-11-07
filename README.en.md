# Motion Logger

> English summary. The full documentation of this repository is in Ukrainian: [README.md](README.md).

A motion logger on a Raspberry Pi 5, built as a custom Buildroot image (`sdcard.img`). The application, board files and image config live in a separate `BR2_EXTERNAL` tree, so upstream Buildroot stays unpatched.

What it does: reads two accelerometers (MPU6050 over I2C, ADXL345 over SPI) through kernel IIO drivers and a GPS module (NEO-6M) over UART/NMEA, detects movement, and writes a JSON Lines log. Two buttons and an RGB LED are part of the board.

Engineering points:
- the application core has no hardware dependency: a sensor interface, a CSV replay driver and a sensor simulator let the whole logic run and be tested without the board;
- tests run under AddressSanitizer, UBSan and ThreadSanitizer; `make check-samples` compares the analysis report with ground truth;
- one Makefile covers native, aarch64 cross and static builds;
- image config is a `savedefconfig` of `raspberrypi5_defconfig` plus own changes; a full build takes about 1.5 hours, an app-only change needs just `make motion_logger-rebuild`;
- BusyBox init script `S99motion_logger` (start/stop/restart/status) with settings in `/etc/default`, checked by a script that runs without the board;
- boot logs, a design doc and a changelog are in the repo.

Stack: C, Buildroot 2026.08, BR2_EXTERNAL, BusyBox, Linux IIO, libgpiod, device tree, Make, Raspberry Pi 5.
