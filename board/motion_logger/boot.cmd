# Скрипт U-Boot для Motion Logger (Buildroot збирає з нього boot.scr командою mkimage).
# U-Boot стартує після прошивки Pi 5 (config.txt: kernel=u-boot.bin), знаходить boot.scr на першому
# розділі карти (distro boot) і виконує цей файл. Змінні devtype, devnum і distro_bootpart виставляє
# сам distro boot, тому номер карти не зашито. fdt_addr це адреса device tree, яку передала
# прошивка: у ньому вже застосовано dtoverlay з config.txt (наш motion-logger-sensors).
#
# Рядок ядра дублює cmdline.txt, бо U-Boot бере bootargs з власного середовища; змінюючи один файл,
# змінюй і другий. Дивись docs/uboot.md.
setenv bootargs "root=/dev/mmcblk0p2 rootwait console=tty1 console=ttyAMA10,115200"
load ${devtype} ${devnum}:${distro_bootpart} ${kernel_addr_r} Image
booti ${kernel_addr_r} - ${fdt_addr}
