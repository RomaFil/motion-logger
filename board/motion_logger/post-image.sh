#!/bin/bash
# Збирає sdcard.img для Motion Logger (Raspberry Pi 5).
# Основа: board/raspberrypi5/post-image.sh з Buildroot 2026.08 + додано D0-оверлей.

set -e

BOARD_DIR="$(dirname "$0")"
GENIMAGE_CFG="${BINARIES_DIR}/genimage.cfg"
GENIMAGE_TMP="${BUILD_DIR}/genimage.tmp"

# Pi 5 із чипом BCM2712 D0 (зокрема модель 16 ГБ): прошивка шукає /overlays/bcm2712d0.dtbo.
# Оверлей збирається разом із ядром, але Buildroot його в образ не кладе, тому беремо з каталогу ядра.
OVL_SRC=$(ls "${BUILD_DIR}"/linux-*/arch/arm/boot/dts/overlays/bcm2712d0.dtbo 2>/dev/null | head -n 1)
if [ -z "${OVL_SRC}" ]; then
	echo "post-image: не знайдено bcm2712d0.dtbo у каталозі збірки ядра" >&2
	exit 1
fi
mkdir -p "${BINARIES_DIR}/rpi-firmware/overlays"
cp "${OVL_SRC}" "${BINARIES_DIR}/rpi-firmware/overlays/"

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
