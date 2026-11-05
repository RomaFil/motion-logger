#!/bin/sh
# Наскрізна перевірка кроків 5.1 і 5.3: motion_logger --replay програє tests/data/accel_sample.csv
# через драйвер, буфер і записувач, а події MOVE_START / MOVE_STOP у лозі збігаються з тими,
# що показує інструмент analyze_recording на тому самому файлі (час у мс, той самий порядок).
# Заодно перевіряє --dump-raw: analyze_recording на сирому файлі дає ті самі події.
# Лише POSIX sh і awk. Запуск: sh tests/check_replay.sh build/motion_logger build/analyze_recording tests/data

set -eu

# Програємо у 50 разів швидше за реальний час: 50 с запису займають ~1 с.
SPEED=50

APP=$1
TOOL=$2
DATA=$3

LOG=$(mktemp /tmp/check_replay_XXXXXX)
EXPECTED=$(mktemp /tmp/check_replay_XXXXXX)
ACTUAL=$(mktemp /tmp/check_replay_XXXXXX)
RAW=$(mktemp /tmp/check_replay_XXXXXX)
RAW_EVENTS=$(mktemp /tmp/check_replay_XXXXXX)
trap 'rm -f "$LOG" "$EXPECTED" "$ACTUAL" "$RAW" "$RAW_EVENTS"' EXIT

# motion_logger дописує в кінець файлу, тож починаємо з порожнього.
: > "$LOG"
SUMMARY=$("$APP" --replay "$DATA/accel_sample.csv" --speed "$SPEED" --log "$LOG" --dump-raw "$RAW")

# Події зі звіту інструмента, у форматі "START 20550" (секунди "20.550" -> мс).
events_of() {
	"$TOOL" "$1" | awk '
		$2 == "s" && $3 == "MOVE" {
			ms = $1 * 1000 + 0.5
			printf "%s %d\n", $4, ms
		}'
}

# Що очікуємо: події з звіту інструмента на вихідному записі.
events_of "$DATA/accel_sample.csv" > "$EXPECTED"

# Що вийшло в лозі: рядки {"t_ms":20550,"src":"accel","event":"MOVE_START"}.
sed -n 's/^{"t_ms":\([0-9]*\),"src":"accel","event":"MOVE_\([A-Z]*\)"}$/\2 \1/p' "$LOG" > "$ACTUAL"

if [ ! -s "$EXPECTED" ]; then
	echo "check_replay: FAIL no events in analyze_recording output (nothing to compare)"
	exit 1
fi

if ! diff -u "$EXPECTED" "$ACTUAL" > /dev/null; then
	echo "check_replay: FAIL events in the log differ from analyze_recording"
	diff -u "$EXPECTED" "$ACTUAL" || true
	exit 1
fi

# Сирий файл --dump-raw це той самий запис: analyze_recording дає на ньому ті самі події.
events_of "$RAW" > "$RAW_EVENTS"
if ! diff -u "$EXPECTED" "$RAW_EVENTS" > /dev/null; then
	echo "check_replay: FAIL events from the --dump-raw file differ from the original recording"
	diff -u "$EXPECTED" "$RAW_EVENTS" || true
	exit 1
fi

# У лозі події та підсумки секунд, сирих відліків нема, і нічого не загублено.
LINES=$(wc -l < "$LOG" | tr -d ' ')
EVENTS=$(wc -l < "$EXPECTED" | tr -d ' ')
ROWS=$(grep -c '^{"t_ms":[0-9]*,"src":"accel","second":[0-9]*,"moving":\(true\|false\),"avg_dev_g":[0-9.]*}$' "$LOG" || true)
# Рядок відкриття файлу ("src":"log") рівно один і стоїть першим.
OPENS=$(grep -c '^{"t_ms":[0-9]*,"src":"log","event":"open","boot_id":\("[0-9a-f-]*"\|null\),"unix_ms":[0-9]*}$' "$LOG" || true)
if [ "$OPENS" != 1 ] || [ "$(head -n 1 "$LOG" | grep -c '"src":"log"')" != 1 ]; then
	echo "check_replay: FAIL expected exactly one open line at the top of the log, found $OPENS"
	exit 1
fi
if [ "$ROWS" -lt 1 ] || [ "$LINES" != "$((EVENTS + ROWS + OPENS))" ]; then
	echo "check_replay: FAIL log has $LINES lines, expected $EVENTS events + per-second rows (found $ROWS) + open line"
	exit 1
fi
case "$SUMMARY" in
*"dropped 0,"*"log errors 0,"*) ;;
*)
	echo "check_replay: FAIL unexpected summary: $SUMMARY"
	exit 1
	;;
esac

echo "check_replay: ok, $EVENTS events in the log match analyze_recording"
