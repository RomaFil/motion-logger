#!/bin/sh
# Перевіряє решту rootfs_overlay без плати: усі скрипти init.d і файли /etc/default (shebang, LF,
# біт виконання, синтаксис), /etc/default/dropbear (ключі з розділу boot вимикають пароль),
# S12mounts і S49ntp (аргументи), genimage.cfg.in (розділ data), busybox.fragment.
# S99motion_logger перевіряє check_init_script.sh.
# Лише POSIX sh. Запуск: sh board/motion_logger/tests/check_overlay.sh
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
BOARD="$HERE/.."
OVERLAY="$BOARD/rootfs_overlay"
WORK=$(mktemp -d /tmp/check_overlay_XXXXXX)
failures=0
trap 'rm -rf "$WORK"' EXIT

check() {
	description=$1
	shift
	if "$@"; then
		echo "  ok   $description"
	else
		echo "  FAIL $description"
		failures=$((failures + 1))
	fi
}
rc() {
	expected=$1
	shift
	"$@" >/dev/null 2>&1
	[ "$?" -eq "$expected" ]
}
CR=$(printf '\r')

echo "init.d: файли"
for f in "$OVERLAY"/etc/init.d/S*; do
	name=$(basename "$f")
	check "$name: shebang #!/bin/sh" sh -c "head -n 1 '$f' | grep -qx '#!/bin/sh'"
	check "$name: рядки Unix" sh -c "! grep -q '$CR' '$f'"
	check "$name: виконуваний" test -x "$f"
	check "$name: синтаксис sh -n" sh -n "$f"
done
for f in "$OVERLAY"/etc/default/*; do
	check "$(basename "$f"): синтаксис sh -n" sh -n "$f"
done

echo "S12mounts, S49ntp: аргументи"
check "S12mounts без аргументу = 1" rc 1 sh "$OVERLAY/etc/init.d/S12mounts"
check "S12mounts bogus = 1" rc 1 sh "$OVERLAY/etc/init.d/S12mounts" bogus
check "S49ntp без аргументу = 1" rc 1 sh "$OVERLAY/etc/init.d/S49ntp"

echo "/etc/default/dropbear: ключі з розділу boot"
# Копія з підміненими /boot і /root, щоб нічого не чіпати на машині розробника.
sed -e "s|/boot/|$WORK/boot/|g" -e "s|/root/|$WORK/root/|g" \
	"$OVERLAY/etc/default/dropbear" >"$WORK/dropbear"
mkdir -p "$WORK/boot"
args_for() { (
	DROPBEAR_ARGS=""
	. "$WORK/dropbear"
	printf '%s' "$DROPBEAR_ARGS"
); }
check "без authorized_keys пароль лишається (аргументів нема)" test -z "$(args_for)"
: >"$WORK/boot/authorized_keys"
check "порожній authorized_keys нічого не вимикає" test -z "$(args_for)"
echo "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIExample roman@laptop" >"$WORK/boot/authorized_keys"
check "з ключем вхід за паролем вимкнено (-s)" test "$(args_for)" = "-s"
check "ключ скопійовано" cmp -s "$WORK/boot/authorized_keys" "$WORK/root/.ssh/authorized_keys"
check "права authorized_keys 600" sh -c "ls -l '$WORK/root/.ssh/authorized_keys' | grep -q '^-rw-------'"
check "права .ssh 700" sh -c "ls -ld '$WORK/root/.ssh' | grep -q '^drwx------'"

echo "образ: розділ data і BusyBox"
check "genimage: образ data.ext4 з міткою data" grep -q 'label = "data"' "$BOARD/genimage.cfg.in"
check "genimage: розділ data у sdcard.img" grep -q 'image = "data.ext4"' "$BOARD/genimage.cfg.in"
check "busybox.fragment: ntpd" grep -qx 'CONFIG_NTPD=y' "$BOARD/busybox.fragment"
check "S12mounts монтує третій розділ" grep -q '^DATA_DEV=/dev/mmcblk0p3$' "$OVERLAY/etc/init.d/S12mounts"

if [ "$failures" -ne 0 ]; then
	echo "check_overlay: FAIL ($failures)"
	exit 1
fi
echo "check_overlay: ok"
