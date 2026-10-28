#!/bin/sh
# Перевіряє запис у tests/data двома способами:
#   1. звіт analyze_recording збігається з еталоном expected_report.txt (нічого не змінилось);
#   2. кожна секунда звіту каже "рух" там, де в ground_truth.txt "walking", і "руху нема" там,
#      де "still" (дві секунди біля меж проміжків не рахуємо: там стан міняється).
# Лише POSIX sh і awk. Запуск: sh tests/check_samples.sh build/analyze_recording tests/data
# З параметром --update переписує еталон expected_report.txt замість порівняння.

set -eu

MARGIN_SECONDS=2

TOOL=$1
DATA=$2
UPDATE=${3:-}

REPORT=$("$TOOL" "$DATA/accel_sample.csv" "$DATA/gps_sample.nmea")

if [ "$UPDATE" = "--update" ]; then
	printf '%s\n' "$REPORT" > "$DATA/expected_report.txt"
	echo "check_samples: expected_report.txt updated"
	exit 0
fi

if ! printf '%s\n' "$REPORT" | diff -u "$DATA/expected_report.txt" - > /dev/null; then
	echo "check_samples: FAIL report differs from expected_report.txt (run: make update-sample-report)"
	printf '%s\n' "$REPORT" | diff -u "$DATA/expected_report.txt" - | head -20
	exit 1
fi

# Таблиця секунд: рядки виду "    21 yes     100 0.3504".
printf '%s\n' "$REPORT" | awk -v margin="$MARGIN_SECONDS" -v truth="$DATA/ground_truth.txt" '
BEGIN {
	segments = 0
	while ((getline line < truth) > 0) {
		if (line ~ /^#/ || line == "") continue
		split(line, part, " ")
		segments++
		start[segments] = part[1]
		stop[segments] = part[2]
		state[segments] = part[3]
	}
	checked = 0
	wrong = 0
}
$1 ~ /^[0-9]+$/ && ($2 == "yes" || $2 == "no") {
	for (i = 1; i <= segments; i++) {
		if ($1 >= start[i] + margin && $1 < stop[i] - margin) {
			expected = (state[i] == "walking") ? "yes" : "no"
			checked++
			if ($2 != expected) {
				printf "check_samples: FAIL second %s says %s, ground truth is %s\n", $1, $2, state[i]
				wrong++
			}
		}
	}
}
END {
	if (checked == 0) {
		print "check_samples: FAIL no seconds were checked"
		exit 1
	}
	if (wrong > 0) exit 1
	printf "check_samples: ok (%d seconds match ground truth, report equals expected)\n", checked
}'
