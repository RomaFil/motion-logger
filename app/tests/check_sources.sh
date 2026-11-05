#!/bin/sh
# Наскрізна перевірка кроку 5.2: motion_logger читає три джерела одночасно, без справжнього заліза.
#   --iio       фальшивий каталог IIO (звичайні файли in_accel_*_raw, in_accel_scale)
#   --iio-aux   другий такий каталог
#   --gps       іменований канал (FIFO), у який подано рядки NMEA з tests/data/gps_sample.nmea
# Після кількох секунд роботи програма отримує SIGTERM. У лозі мають бути рядки accel, accel2
# і gps, а підсумок не має містити втрат і помилок запису.
# Лише POSIX sh. Запуск: sh tests/check_sources.sh build/motion_logger tests/data

set -eu

APP=$1
DATA=$2

# Скільки секунд працює програма: з розрахунком на хоча б дві межі секунди у фільтрі руху.
RUN_SECONDS=3
# Рядків RMC у файлі NMEA: стільки ж рядків gps очікуємо в лозі.
RMC_LINES=$(grep -c 'RMC' "$DATA/gps_sample.nmea")
# З них зі статусом A (є позиція) і V (нема): стільки ж рядків fix:true і fix:false очікуємо в лозі.
# Числа беремо з самого файлу, а не з коду: файл можна замінити іншим записом (крок 1.6).
RMC_FIX_LINES=$(grep -c '^\$G[A-Z]RMC,[^,]*,A,' "$DATA/gps_sample.nmea" || true)
RMC_NO_FIX_LINES=$(grep -c '^\$G[A-Z]RMC,[^,]*,V,' "$DATA/gps_sample.nmea" || true)

WORK=$(mktemp -d /tmp/check_sources_XXXXXX)
PID=
cleanup() {
	if [ -n "$PID" ]; then
		kill -KILL "$PID" 2> /dev/null || true
	fi
	rm -rf "$WORK"
}
trap cleanup EXIT

fail() {
	echo "check_sources: FAIL $1"
	exit 1
}

# Каталог пристрою IIO зі спільним масштабом 9,80665 (одна одиниця raw це рівно 1 g).
make_iio() {
	mkdir "$1"
	printf '%s\n' "$2" > "$1/in_accel_x_raw"
	printf '%s\n' "$3" > "$1/in_accel_y_raw"
	printf '%s\n' "$4" > "$1/in_accel_z_raw"
	printf '9.80665\n' > "$1/in_accel_scale"
}

make_iio "$WORK/main" 0 0 1
make_iio "$WORK/aux" 0 0 1
# Гіроскоп і температура в каталозі основного пристрою (як у MPU6050): 1 град/с на одиницю raw,
# температура (raw 5 + зсув 20) * 1000 мілі-°C = 25 °C.
printf '2\n' > "$WORK/main/in_anglvel_x_raw"
printf -- '-1\n' > "$WORK/main/in_anglvel_y_raw"
printf '0\n' > "$WORK/main/in_anglvel_z_raw"
printf '0.017453292519943295\n' > "$WORK/main/in_anglvel_scale"
printf '5\n' > "$WORK/main/in_temp_raw"
printf '1000\n' > "$WORK/main/in_temp_scale"
printf '20\n' > "$WORK/main/in_temp_offset"
mkfifo "$WORK/gps"
LOG="$WORK/log.jsonl"
SUMMARY="$WORK/summary.txt"

# Відкриваємо канал на читання й запис одразу: поки ми його тримаємо, читач не побачить кінця даних.
exec 3<> "$WORK/gps"

"$APP" --iio "$WORK/main" --iio-aux "$WORK/aux" --iio-gyro "$WORK/main" --gps "$WORK/gps" \
	--log "$LOG" > "$SUMMARY" &
PID=$!

cat "$DATA/gps_sample.nmea" >&3
sleep "$RUN_SECONDS"

kill -TERM "$PID"
STATUS=0
wait "$PID" || STATUS=$?
PID=
exec 3>&-

[ "$STATUS" -eq 0 ] || fail "motion_logger exited with status $STATUS after SIGTERM"

count_lines() {
	grep -c "$1" "$LOG" || true
}

ACCEL_ROWS=$(count_lines '"src":"accel","second"')
AUX_LINES=$(count_lines '"src":"accel2"')
GPS_LINES=$(count_lines '"src":"gps"')
GPS_FIX_LINES=$(count_lines '"src":"gps","fix":true,"lat":')
GPS_NO_FIX_LINES=$(count_lines '"src":"gps","fix":false')

[ "$ACCEL_ROWS" -ge 1 ] || fail "no per-second accel rows in the log"
[ "$AUX_LINES" -ge 1 ] || fail "no accel2 lines in the log"
[ "$GPS_LINES" -eq "$RMC_LINES" ] || fail "expected $RMC_LINES gps lines, got $GPS_LINES"
[ "$GPS_FIX_LINES" -eq "$RMC_FIX_LINES" ] ||
	fail "expected $RMC_FIX_LINES gps lines with a fix position, got $GPS_FIX_LINES"
[ "$GPS_NO_FIX_LINES" -eq "$RMC_NO_FIX_LINES" ] ||
	fail "expected $RMC_NO_FIX_LINES gps lines without a fix, got $GPS_NO_FIX_LINES"

# Один акселерометр лежить нерухомо на столі: жодної події руху.
[ "$(count_lines '"event":"MOVE_')" -eq 0 ] || fail "motion events for a board lying still"

# Перший рядок: відкриття файлу.
head -n 1 "$LOG" | grep -q '"src":"log","event":"open"' || fail "no open line at the top of the log"

# Додатковий акселерометр: не більше рядка на секунду і значення z = 1 g.
[ "$AUX_LINES" -le $((RUN_SECONDS + 1)) ] || fail "accel2 is written more than once per second"
grep -q '"src":"accel2","x_g":0.0000,"y_g":0.0000,"z_g":1.0000' "$LOG" ||
	fail "accel2 line has unexpected values"

# Гіроскоп: не більше рядка на секунду, значення переведені в град/с, є температура 25 °C.
GYRO_LINES=$(count_lines '"src":"gyro"')
[ "$GYRO_LINES" -ge 1 ] || fail "no gyro lines in the log"
[ "$GYRO_LINES" -le $((RUN_SECONDS + 1)) ] || fail "gyro is written more than once per second"
grep -q '"src":"gyro","x_dps":2.00,"y_dps":-1.00,"z_dps":0.00,"temp_c":25.0}' "$LOG" ||
	fail "gyro line has unexpected values"

# Додаткові поля GPS (час UTC з супутників, висота, курс, HDOP) беруться з рядків RMC і GGA файла NMEA.
# Форма, а не значення: tests/data з feature/uboot-wifi це справжній запис (GPS із підміною), не синтетика.
grep -q '"fix":true,.*"utc":"20[0-9][0-9]-[01][0-9]-[0-3][0-9]T[0-2][0-9]:[0-5][0-9]:[0-5][0-9]Z","alt_m":-\{0,1\}[0-9][0-9.]*,"course_deg":[0-9][0-9.]*,"hdop":[0-9][0-9.]*}' "$LOG" ||
	fail "gps line with utc, altitude, course and hdop not found"
# Без фіксації часу UTC немає: модуль міг узяти його зі своєї пам'яті.
if grep '"fix":false' "$LOG" | grep -q '"utc"'; then
	fail "gps line without a fix has a utc time"
fi

case "$(cat "$SUMMARY")" in
*"dropped 0,"*"log errors 0,"*) ;;
*) fail "unexpected summary: $(cat "$SUMMARY")" ;;
esac

echo "check_sources: ok, $ACCEL_ROWS accel rows, $AUX_LINES accel2, $GPS_LINES gps lines"
