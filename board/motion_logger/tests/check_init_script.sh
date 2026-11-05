#!/bin/sh
# Перевіряє rootfs_overlay/etc/init.d/S99motion_logger за чеклістом презентації «Init» (слайд 43):
#   start двічі дає один процес; stop після start прибирає процес; restart працює;
#   status повертає 0 / 3; застарілий PID-файл не заважає; процес, що ігнорує SIGTERM, вбивається;
#   параметри з /etc/default передаються демону; наглядач перезапускає демона після аварії
#   (код не 0) і не перезапускає після успішного завершення; сирота після kill -9 наглядача
#   прибирається при наступному start.
# Процеси, що лишились без батьків, прибирає init (PID 1). У контейнері без справжнього init вони
# стають зомбі й лишаються «живими» для kill -0: там запускайте перевірку під subreaper
# (prctl PR_SET_CHILD_SUBREAPER), інакше перевірки «зник» дадуть FAIL.
# Скрипт копіюється в тимчасовий каталог із підміною шляхів і запускається з фейковим демоном,
# тому нічого на машині розробника не чіпається. Лише POSIX sh. Запуск: sh check_init_script.sh
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
SCRIPT="$HERE/../rootfs_overlay/etc/init.d/S99motion_logger"
WORK=$(mktemp -d /tmp/check_init_XXXXXX)
failures=0

cleanup() {
	# Прибираємо можливі залишки фейкових демонів.
	for f in "$WORK"/*.pid; do
		[ -f "$f" ] && kill -9 "$(cat "$f")" 2>/dev/null
	done
	rm -rf "$WORK"
}
trap cleanup EXIT

check() {
	# check "опис" команда...: команда має завершитись з кодом 0
	description=$1
	shift
	if "$@"; then
		echo "  ok   $description"
	else
		echo "  FAIL $description"
		failures=$((failures + 1))
	fi
}

# Фейкові демони: один завершується по SIGTERM, другий SIGTERM ігнорує.
cat >"$WORK/good" <<'EOF'
#!/bin/sh
echo "$@" >"$0.args"
trap 'exit 0' TERM
while :; do sleep 1 & wait $!; done
EOF
cat >"$WORK/stubborn" <<'EOF'
#!/bin/sh
trap '' TERM
while :; do sleep 1; done
EOF
# Перший падає одразу (код 1), другий завершується успішно (код 0); обидва рахують запуски.
printf '#!/bin/sh\necho run >>"$0.runs"\nexit 1\n' >"$WORK/crasher"
printf '#!/bin/sh\necho run >>"$0.runs"\nexit 0\n' >"$WORK/oneshot"
chmod +x "$WORK/good" "$WORK/stubborn" "$WORK/crasher" "$WORK/oneshot"

# Копія скрипта служби з підміненими шляхами та коротшим тайм-аутом.
make_script() {
	daemon=$1
	sed -e "s|^DAEMON=.*|DAEMON=$daemon|" \
		-e "s|^PIDFILE=.*|PIDFILE=$WORK/test.pid|" \
		-e "s|^CHILD_PIDFILE=.*|CHILD_PIDFILE=$WORK/child.pid|" \
		-e "s|^RESTART_DELAY=.*|RESTART_DELAY=1|" \
		-e "s|^RESTART_DELAY_MAX=.*|RESTART_DELAY_MAX=1|" \
		-e "s|^LOGFILE=.*|LOGFILE=$WORK/test.log|" \
		-e "s|^DEBUG_TTY=.*|DEBUG_TTY=$WORK/no_such_tty|" \
		-e "s|^STOP_TIMEOUT=.*|STOP_TIMEOUT=1|" \
		-e "s|^IIO_ROOT=.*|IIO_ROOT=$WORK/iio|" \
		-e "s|^IIO_WAIT=.*|IIO_WAIT=1|" \
		-e "s|/etc/default/\$NAME|$WORK/default|g" \
		"$SCRIPT" >"$WORK/S99"
	chmod +x "$WORK/S99"
}

alive() { kill -0 "$1" 2>/dev/null; }
gone() { ! kill -0 "$1" 2>/dev/null; }
rc() { # rc очікуваний_код команда...: перевіряє код виходу
	expected=$1
	shift
	"$@" >/dev/null 2>&1
	[ "$?" -eq "$expected" ]
}

echo "S99motion_logger: файл"
check "shebang #!/bin/sh" sh -c "head -n 1 '$SCRIPT' | grep -qx '#!/bin/sh'"
check "рядки Unix (нема CR)" sh -c "! grep -q \"$(printf '\r')\" '$SCRIPT'"
check "файл виконуваний" test -x "$SCRIPT"
check "синтаксис sh -n" sh -n "$SCRIPT"

echo "S99motion_logger: життєвий цикл (фейковий демон, що слухає SIGTERM)"
make_script "$WORK/good"
check "status до запуску = 3" rc 3 "$WORK/S99" status
check "start = 0" rc 0 "$WORK/S99" start
pid1=$(cat "$WORK/test.pid")
check "процес живий після start" alive "$pid1"
check "start удруге = 0 (не помилка)" rc 0 "$WORK/S99" start
pid2=$(cat "$WORK/test.pid")
check "start удруге не створює другий процес (той самий PID)" test "$pid1" = "$pid2"
check "status = 0" rc 0 "$WORK/S99" status
check "stop = 0" rc 0 "$WORK/S99" stop
check "процес зник після stop" gone "$pid1"
check "PID-файл прибрано" test ! -e "$WORK/test.pid"
check "stop удруге = 0" rc 0 "$WORK/S99" stop
check "status після stop = 3" rc 3 "$WORK/S99" status
check "restart = 0" rc 0 "$WORK/S99" restart
pid3=$(cat "$WORK/test.pid")
check "restart дав живий процес" alive "$pid3"
check "restart дав новий PID" test "$pid3" != "$pid1"

echo "S99motion_logger: аварійне завершення наглядача і застарілий PID-файл"
sleep 1
orphan=$(cat "$WORK/child.pid")
check "демон живий під наглядачем" alive "$orphan"
kill -9 "$pid3"
sleep 1
check "status після kill -9 = 3" rc 3 "$WORK/S99" status
check "start після аварії = 0" rc 0 "$WORK/S99" start
pid4=$(cat "$WORK/test.pid")
check "після аварії стартує новий живий процес" alive "$pid4"
check "сирота від убитого наглядача прибрана" gone "$orphan"
"$WORK/S99" stop >/dev/null 2>&1

echo "S99motion_logger: наглядач перезапускає після аварії"
make_script "$WORK/crasher"
rm -f "$WORK/crasher.runs"
"$WORK/S99" start >/dev/null 2>&1
sleep 4
runs=$(wc -l <"$WORK/crasher.runs" 2>/dev/null || echo 0)
check "демон, що падає, перезапущено (запусків: $runs)" test "$runs" -ge 2
check "status під час перезапусків = 0" rc 0 "$WORK/S99" status
pid6=$(cat "$WORK/test.pid")
check "stop = 0" rc 0 "$WORK/S99" stop
check "наглядач зник після stop" gone "$pid6"
runs_after=$(wc -l <"$WORK/crasher.runs")
sleep 2
check "після stop перезапусків нема" test "$(wc -l <"$WORK/crasher.runs")" -eq "$runs_after"

echo "S99motion_logger: успішне завершення не перезапускається"
make_script "$WORK/oneshot"
rm -f "$WORK/oneshot.runs"
"$WORK/S99" start >/dev/null 2>&1
sleep 3
check "запуск рівно один" test "$(wc -l <"$WORK/oneshot.runs")" -eq 1
check "status після успішного завершення = 3" rc 3 "$WORK/S99" status
"$WORK/S99" stop >/dev/null 2>&1

echo "S99motion_logger: параметри з /etc/default"
make_script "$WORK/good"
printf 'MOTION_LOGGER_OPTS="--replay /x.csv --speed 50"\n' >"$WORK/default"
rm -f "$WORK/good.args"
"$WORK/S99" start >/dev/null 2>&1
sleep 1
check "демон отримав параметри" grep -qx -e '--replay /x.csv --speed 50' "$WORK/good.args"
"$WORK/S99" stop >/dev/null 2>&1
rm -f "$WORK/default"

echo "S99motion_logger: датчики за ім'ям (@ім'я) і частота датчика"
# Фальшиве дерево IIO: номери навмисно в «неправильному» порядку, щоб було видно, що шукаємо за ім'ям.
mkdir -p "$WORK/iio/iio:device0" "$WORK/iio/iio:device1"
echo adxl345 >"$WORK/iio/iio:device0/name"
echo mpu6050 >"$WORK/iio/iio:device1/name"
echo 50 >"$WORK/iio/iio:device1/sampling_frequency"
printf 'MOTION_LOGGER_OPTS="--iio @mpu6050 --iio-rate 100 --iio-aux @adxl345 --log /x.jsonl"\n' >"$WORK/default"
printf 'MOTION_LOGGER_IIO_RATES="mpu6050:100"\n' >>"$WORK/default"
rm -f "$WORK/good.args"
make_script "$WORK/good"
check "start із @ім'ям = 0" rc 0 "$WORK/S99" start
sleep 1
check "@mpu6050 і @adxl345 замінено на каталоги за ім'ям" grep -qx -e "--iio $WORK/iio/iio:device1 --iio-rate 100 --iio-aux $WORK/iio/iio:device0 --log /x.jsonl" "$WORK/good.args"
check "частота MPU6050 виставлена в sampling_frequency" grep -qx 100 "$WORK/iio/iio:device1/sampling_frequency"
check "датчик, якого нема в списку частот, не чіпаний" test ! -e "$WORK/iio/iio:device0/sampling_frequency"
"$WORK/S99" stop >/dev/null 2>&1

printf 'MOTION_LOGGER_OPTS="--iio @nosuch --log /x.jsonl"\n' >"$WORK/default"
check "невідоме ім'я датчика: start = 1" rc 1 "$WORK/S99" start
check "невідоме ім'я: процес не запущено" test ! -e "$WORK/test.pid"
check "невідоме ім'я: status = 3" rc 3 "$WORK/S99" status
rm -f "$WORK/default"

echo "S99motion_logger: демон, що ігнорує SIGTERM"
make_script "$WORK/stubborn"
"$WORK/S99" start >/dev/null 2>&1
sleep 1
pid5=$(cat "$WORK/test.pid")
child5=$(cat "$WORK/child.pid")
check "stubborn живий" alive "$child5"
check "stop = 0" rc 0 "$WORK/S99" stop
check "наглядач вбито через SIGKILL" gone "$pid5"
check "stubborn вбито через SIGKILL" gone "$child5"

echo "S99motion_logger: аргументи"
check "невідомий аргумент = 1" rc 1 "$WORK/S99" bogus
check "без аргументу = 1" rc 1 "$WORK/S99"

if [ "$failures" -ne 0 ]; then
	echo "check_init_script: FAIL ($failures)"
	exit 1
fi
echo "check_init_script: ok"
