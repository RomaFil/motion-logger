#!/bin/sh
# Звіряє linux.fragment із .config зібраного ядра: кожен рядок CONFIG_X=значення або
# "# CONFIG_X is not set" у фрагменті має збігатися з підсумковим .config. Якщо дефолт іншої версії
# ядра перекриє наш рядок (або символ перейменують), перевірка впаде.
# Лише POSIX sh. Запуск: sh board/motion_logger/tests/check_kernel_fragment.sh <build/linux-custom/.config>
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
FRAGMENT="$HERE/../linux.fragment"
CONFIG=${1:-}

if [ -z "$CONFIG" ] || [ ! -r "$CONFIG" ]; then
	echo "Використання: $0 <шлях до .config ядра (output/build/linux-custom/.config)>" >&2
	exit 2
fi

failures=0
checked=0
while IFS= read -r line; do
	case "$line" in
	CONFIG_*=* | "# CONFIG_"*" is not set")
		checked=$((checked + 1))
		if grep -qxF "$line" "$CONFIG"; then
			echo "  ok   $line"
		else
			symbol=$(printf '%s' "$line" | sed -e 's/^# //' -e 's/[= ].*//')
			actual=$(grep -E "^(# )?${symbol}[= ]" "$CONFIG" | head -n 1)
			echo "  FAIL $line (у .config: ${actual:-символа нема})"
			failures=$((failures + 1))
		fi
		;;
	esac
done <"$FRAGMENT"

if [ "$checked" -eq 0 ]; then
	echo "check_kernel_fragment: FAIL (у $FRAGMENT нема жодного рядка CONFIG_)"
	exit 1
fi
if [ "$failures" -ne 0 ]; then
	echo "check_kernel_fragment: FAIL ($failures з $checked)"
	exit 1
fi
echo "check_kernel_fragment: ok ($checked рядків)"
