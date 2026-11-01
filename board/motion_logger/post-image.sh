#!/bin/bash
# Збирає sdcard.img для Motion Logger (Raspberry Pi 5).
# Основа: board/raspberrypi5/post-image.sh з Buildroot 2026.08 + додано D0-оверлей.

set -e

BOARD_DIR="$(dirname "$0")"
GENIMAGE_CFG="${BINARIES_DIR}/genimage.cfg"
GENIMAGE_TMP="${BUILD_DIR}/genimage.tmp"

# config.txt і cmdline.txt беремо з репозиторію щоразу: пакет rpi-firmware копіює їх у images/ лише коли
# сам перебудовується, а Buildroot не помічає змін у файлах, шлях до яких заданий у defconfig. Через це
# перша збірка з датчиками (29.10.2026) отримала старий config.txt без dtparam і dtoverlay.
mkdir -p "${BINARIES_DIR}/rpi-firmware"
cp "${BOARD_DIR}/config.txt" "${BOARD_DIR}/cmdline.txt" "${BINARIES_DIR}/rpi-firmware/"

# Wi-Fi: /etc/init.d/S41wifi читає мережі з wifi.txt на розділі boot. Файл поза git: беремо його з машини
# збірки ($WIFI_SECRET або ~/workspace/wifi.secret) і кладемо на boot-розділ образу. Нема файлу, образ
# збереться без Wi-Fi-налаштувань (покласти wifi.txt на картку вручну, зразок board/motion_logger/wifi.txt.example).
WIFI_SECRET="${WIFI_SECRET:-${HOME}/workspace/wifi.secret}"
rm -f "${BINARIES_DIR}/rpi-firmware/wifi.txt"
if [ -r "${WIFI_SECRET}" ]; then
	install -m 600 "${WIFI_SECRET}" "${BINARIES_DIR}/rpi-firmware/wifi.txt"
	echo "post-image: wifi.txt на boot-розділі взято з ${WIFI_SECRET} (вміст не друкуємо)"
else
	echo "post-image: УВАГА: нема ${WIFI_SECRET}, у образі не буде wifi.txt" >&2
fi

# Pi 5 із чипом BCM2712 D0 (зокрема модель 16 ГБ): прошивка шукає /overlays/bcm2712d0.dtbo.
# Оверлей збирається разом із ядром, але Buildroot його в образ не кладе, тому беремо з каталогу ядра.
OVL_SRC=$(ls "${BUILD_DIR}"/linux-*/arch/arm/boot/dts/overlays/bcm2712d0.dtbo 2>/dev/null | head -n 1)
if [ -z "${OVL_SRC}" ]; then
	echo "post-image: не знайдено bcm2712d0.dtbo у каталозі збірки ядра" >&2
	exit 1
fi
mkdir -p "${BINARIES_DIR}/rpi-firmware/overlays"
cp "${OVL_SRC}" "${BINARIES_DIR}/rpi-firmware/overlays/"

# UART2 (GPIO4/5) для GPS: стандартний оверлей ядра теж лежить лише в каталозі збірки ядра.
UART2_SRC=$(ls "${BUILD_DIR}"/linux-*/arch/arm/boot/dts/overlays/uart2-pi5.dtbo 2>/dev/null | head -n 1)
if [ -z "${UART2_SRC}" ]; then
	echo "post-image: не знайдено uart2-pi5.dtbo у каталозі збірки ядра" >&2
	exit 1
fi
cp "${UART2_SRC}" "${BINARIES_DIR}/rpi-firmware/overlays/"

# Власний оверлей датчиків (MPU6050 на I2C, ADXL345 на SPI): компілюємо dtc із зібраного ядра,
# ключ -@ лишає символи, потрібні прошивці Pi для застосування оверлея.
DTC=$(ls "${BUILD_DIR}"/linux-*/scripts/dtc/dtc 2>/dev/null | head -n 1)
if [ -z "${DTC}" ]; then
	echo "post-image: не знайдено dtc у каталозі збірки ядра" >&2
	exit 1
fi
"${DTC}" -@ -I dts -O dtb \
	-o "${BINARIES_DIR}/rpi-firmware/overlays/motion-logger-sensors.dtbo" \
	"${BOARD_DIR}/dts/motion-logger-sensors.dts"

# Список файлів розділу boot: dtb, усе з rpi-firmware (config.txt, cmdline.txt, overlays) і ядро
FILES=()
for i in "${BINARIES_DIR}"/*.dtb "${BINARIES_DIR}"/rpi-firmware/*; do
	FILES+=( "${i#${BINARIES_DIR}/}" )
done

KERNEL=$(sed -n 's/^kernel=//p' "${BINARIES_DIR}/rpi-firmware/config.txt")
FILES+=( "${KERNEL}" )

BOOT_FILES=$(printf '\\t\\t\\t"%s",\\n' "${FILES[@]}")
sed "s|#BOOT_FILES#|${BOOT_FILES}|" "${BOARD_DIR}/genimage.cfg.in" \
	> "${GENIMAGE_CFG}"

# Порожній rootpath: корінь файлової системи вже зібрано в rootfs.ext4
trap 'rm -rf "${ROOTPATH_TMP}"' EXIT
ROOTPATH_TMP="$(mktemp -d)"

rm -rf "${GENIMAGE_TMP}"

genimage \
	--rootpath "${ROOTPATH_TMP}"   \
	--tmppath "${GENIMAGE_TMP}"    \
	--inputpath "${BINARIES_DIR}"  \
	--outputpath "${BINARIES_DIR}" \
	--config "${GENIMAGE_CFG}"

exit $?
