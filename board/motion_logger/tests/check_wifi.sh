#!/bin/sh
# Перевіряє команду wifi (rootfs_overlay/usr/sbin/wifi) і службу S41wifi без плати: файл мереж лежить
# у тимчасовому каталозі, wpa_passphrase / wpa_cli / служба підмінені фейками, нічого не монтується.
# Лише POSIX sh. Запуск: sh board/motion_logger/tests/check_wifi.sh
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
OVERLAY="$HERE/../rootfs_overlay"
WIFI="$OVERLAY/usr/sbin/wifi"
SERVICE_SCRIPT="$OVERLAY/etc/init.d/S41wifi"
TEMPLATE="$HERE/../wpa_supplicant.conf"
WORK=$(mktemp -d /tmp/check_wifi_XXXXXX)
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

# Фейки: wpa_passphrase поводиться як справжній (пароль зі stdin, 8..63 символи, помилка в stdout),
# wpa_cli каже «wpa_supplicant не працює», служба записує, з чим її викликали.
mkdir -p "$WORK/bin"
cat >"$WORK/bin/wpa_passphrase" <<'EOF'
#!/bin/sh
if [ $# -ge 2 ]; then pass=$2; else IFS= read -r pass; fi
if [ ${#pass} -lt 8 ] || [ ${#pass} -gt 63 ]; then
	echo "Passphrase must be 8..63 characters"
	exit 1
fi
printf 'network={\n\tssid="%s"\n\t#psk="%s"\n\tpsk=%s\n}\n' "$1" "$pass" "$(printf '%s' "$1$pass" | md5sum | cut -c1-32)"
EOF
printf '#!/bin/sh\nexit 1\n' >"$WORK/bin/wpa_cli"
printf '#!/bin/sh\necho "$@" >>"%s/service.calls"\n' "$WORK" >"$WORK/service"
chmod +x "$WORK/bin/"* "$WORK/service"

CONF="$WORK/wpa_supplicant.conf"
run_wifi() {
	PATH="$WORK/bin:$PATH" WIFI_CONF="$CONF" WIFI_BOOT_DIR="" WIFI_SERVICE="$WORK/service" \
		sh "$WIFI" "$@"
}
count() { # count рядок: скільки разів рядок трапляється у файлі мереж як окремий рядок (без відступу)
	sed 's/^[[:space:]]*//' "$CONF" | grep -cxF -e "$1"
}
listed() { run_wifi list | grep -qxF -e "$1"; }
not_listed() { ! listed "$1"; }

for f in "$WIFI" "$SERVICE_SCRIPT"; do
	name=$(basename "$f")
	echo "$name: файл"
	check "shebang #!/bin/sh" sh -c "head -n 1 '$f' | grep -qx '#!/bin/sh'"
	check "рядки Unix (нема CR)" sh -c "! grep -q \"$(printf '\r')\" '$f'"
	check "файл виконуваний" test -x "$f"
	check "синтаксис sh -n" sh -n "$f"
done

echo "wifi: додавання"
check "list без файлу: порожньо, код 0" sh -c "[ -z \"\$(WIFI_CONF='$CONF' sh '$WIFI' list)\" ]"
check "add з паролем = 0" rc 0 run_wifi add home secret123
check "файл створено із заголовком ctrl_interface" grep -q '^ctrl_interface=' "$CONF"
check "у файлі є country" grep -q '^country=' "$CONF"
check "ssid записано" test "$(count 'ssid="home"')" -eq 1
check "ключ psk= записано" grep -q '^[[:space:]]*psk=[0-9a-f]' "$CONF"
check "відкритого пароля у файлі нема" sh -c "! grep -q secret123 '$CONF'"
check "після add служба стартує (wpa_supplicant не працює)" grep -qx start "$WORK/service.calls"
check "list показує мережу" listed home
check "пароль зі stdin = 0" sh -c "printf 'stdinpass1\n' | PATH='$WORK/bin:$PATH' WIFI_CONF='$CONF' WIFI_BOOT_DIR= WIFI_SERVICE='$WORK/service' sh '$WIFI' add 'My Net' >/dev/null 2>&1"
check "SSID з пробілом у list" listed "My Net"
psk_before=$(grep -A2 'ssid="home"' "$CONF" | grep 'psk=')
check "повторний add тієї ж мережі = 0" rc 0 run_wifi add home newsecret99
check "мережа не задублювалась" test "$(count 'ssid="home"')" -eq 1
check "пароль оновлено" test "$(grep -A2 'ssid="home"' "$CONF" | grep 'psk=')" != "$psk_before"
check "інша мережа на місці" listed "My Net"
cp "$CONF" "$WORK/before"
check "короткий пароль = 1" rc 1 run_wifi add short 1234
check "після помилки файл не змінився" cmp -s "$CONF" "$WORK/before"
check "порожній SSID = 1" rc 1 run_wifi add "" secret123
check "SSID довший за 32 = 1" rc 1 run_wifi add 123456789012345678901234567890123 secret123
check "add без SSID = 1" rc 1 run_wifi add

echo "wifi: видалення"
check "remove = 0" rc 0 run_wifi remove home
check "мережі нема в list" not_listed home
check "блоку нема у файлі" test "$(count 'ssid="home"')" -eq 0
check "інша мережа лишилась" listed "My Net"
check "заголовок лишився" grep -q '^ctrl_interface=' "$CONF"
check "remove невідомої = 1" rc 1 run_wifi remove nosuch

echo "wifi: файл із Windows (CRLF, пароль відкритим текстом)"
printf 'country=UA\r\n# коментар\r\nnetwork={\r\n\tssid="office"\r\n\tpsk="plaintext1"\r\n}\r\nnetwork={\r\n\tssid="cafe"\r\n\tkey_mgmt=NONE\r\n}\r\n' >"$CONF"
check "list бачить обидві" sh -c "[ \"\$(WIFI_CONF='$CONF' sh '$WIFI' list | tr '\n' ' ')\" = 'office cafe ' ]"
check "remove office = 0" rc 0 run_wifi remove office
check "office прибрано, cafe лишилась" sh -c "[ \"\$(WIFI_CONF='$CONF' sh '$WIFI' list)\" = cafe ]"
check "коментар лишився" grep -q '^# коментар$' "$CONF"
check "шаблон з образу: жодної мережі в list" sh -c "[ -z \"\$(WIFI_CONF='$TEMPLATE' sh '$WIFI' list)\" ]"

echo "wifi: аргументи"
check "без аргументів = 1" rc 1 run_wifi
check "невідома команда = 1" rc 1 run_wifi bogus

echo "S41wifi: без мереж (шаблон з образу) не вмикає Wi-Fi"
cp "$TEMPLATE" "$WORK/boot.conf"
sed -e "s|^CONF=.*|CONF=$WORK/boot.conf|" \
	-e "s|^WPA_PIDFILE=.*|WPA_PIDFILE=$WORK/wpa.pid|" \
	-e "s|^DHCP_PIDFILE=.*|DHCP_PIDFILE=$WORK/dhcp.pid|" \
	-e "s|^\([[:space:]]*\)grep -q \" \$BOOT_DIR vfat \" /proc/mounts && return 0|\1return 0|" \
	-e "s|/etc/default/wifi|$WORK/no_default|g" \
	-e "s|modprobe|$WORK/modprobe_called|" \
	"$SERVICE_SCRIPT" >"$WORK/S41"
chmod +x "$WORK/S41"
check "status до старту = 3" rc 3 "$WORK/S41" status
check "start без мереж = 0" rc 0 "$WORK/S41" start
check "модуль brcmfmac не вантажився" test ! -e "$WORK/modprobe_called"
check "status після start без мереж = 3" rc 3 "$WORK/S41" status
check "stop = 0" rc 0 "$WORK/S41" stop
check "невідомий аргумент = 1" rc 1 "$WORK/S41" bogus

if [ "$failures" -ne 0 ]; then
	echo "check_wifi: FAIL ($failures)"
	exit 1
fi
echo "check_wifi: ok"
