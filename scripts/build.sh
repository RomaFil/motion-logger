#!/bin/bash
# Збирає sdcard.img для Motion Logger: обгортка над make Buildroot з O= і BR2_EXTERNAL (крок 6.2).
#
#   scripts/build.sh config   завантажити configs/motion_logger_defconfig у каталог збірки
#   scripts/build.sh build    config (якщо ще не було) і повна збірка
#   scripts/build.sh app      скинути збірку лише застосунку й зібрати образ заново
#   scripts/build.sh info     розмір і SHA-256 готового образу
#
# Змінні середовища (усі необов'язкові):
#   BUILDROOT_DIR  каталог Buildroot 2026.08, лише upstream (за замовчуванням ../buildroot)
#   BUILD_DIR      каталог збірки O= (за замовчуванням ../build-pi5)
#   JOBS           кількість паралельних завдань make (за замовчуванням 2)
#   LOW_PRIORITY   1: запускати збірку під nice 19 (спільна машина, наприклад VPS)
#   WIFI_SECRET    файл Wi-Fi для boot-розділу (див. docs/wifi.md)
#
# Пакет motion_logger бере сирці з цього репозиторію (SITE_METHOD = local) і сам не помічає змін у них,
# тому після правки app/ користуйтеся командою app, а не голим make.

set -euo pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)
BUILDROOT_DIR=$(realpath -m "${BUILDROOT_DIR:-${REPO}/../buildroot}")
BUILD_DIR=$(realpath -m "${BUILD_DIR:-${REPO}/../build-pi5}")
JOBS="${JOBS:-2}"

usage() {
	sed -n '2,16p' "$0" | sed 's/^# \{0,1\}//'
	exit 1
}

config() {
	if [ ! -f "${BUILDROOT_DIR}/Makefile" ]; then
		echo "build.sh: нема Buildroot у ${BUILDROOT_DIR} (git clone --branch 2026.08 https://gitlab.com/buildroot.org/buildroot.git)" >&2
		exit 1
	fi
	make -C "${BUILDROOT_DIR}" O="${BUILD_DIR}" BR2_EXTERNAL="${REPO}" motion_logger_defconfig
}

info() {
	local img="${BUILD_DIR}/images/sdcard.img"
	if [ ! -f "${img}" ]; then
		echo "build.sh: нема ${img}" >&2
		exit 1
	fi
	ls -l "${img}"
	sha256sum "${img}"
}

# Після config Buildroot пам'ятає BR2_EXTERNAL у каталозі збірки, тому далі make викликається звідти.
build() {
	local runner=()
	[ -f "${BUILD_DIR}/.config" ] || config
	[ "${LOW_PRIORITY:-0}" = 1 ] && runner=(nice -n 19)
	${runner[@]+"${runner[@]}"} make -C "${BUILD_DIR}" -j"${JOBS}"
	info
}

case "${1:-}" in
config)
	config
	;;
build)
	build
	;;
app)
	[ -f "${BUILD_DIR}/.config" ] || config
	make -C "${BUILD_DIR}" motion_logger-dirclean
	build
	;;
info)
	info
	;;
*)
	usage
	;;
esac
