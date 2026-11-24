#!/bin/sh
# Glag on a computer: the control tests, then every piece rendered and checked for level, then
# every render's fingerprint checked against golden.sha1 (so nothing changes the sound by accident).
# usage: sh firmware/host/run_tests.sh [OUTDIR]
#   UPDATE_GOLDEN=1 sh firmware/host/run_tests.sh   writes new fingerprints (after a change meant
#   to alter the sound, once the change has been listened to). Fingerprints hold for one compiler
#   and machine; another may round differently and need its own.
set -e
cd "$(dirname "$0")"
OUT=${1:-build}
mkdir -p "$OUT"
CC=${CC:-cc}
$CC -O2 -Wall -Wextra -I../glag -o "$OUT/ui_test" ui_test.c ../glag/glag_ui.c ../glag/glag_engine.c -lm
$CC -O2 -Wall -I../glag -o "$OUT/render" render.c ../glag/glag_engine.c -lm
$CC -O2 -Wall -DGL_DEMO_PIECES -I../glag -o "$OUT/render_demo" render.c ../glag/glag_engine.c -lm
"$OUT/ui_test"
$CC -O2 -Wall -DGL_TEST_HOOKS -I../glag -o "$OUT/hall_test" hall_test.c ../glag/glag_ui.c ../glag/glag_engine.c -lm
"$OUT/hall_test" | tail -1
for i in 0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do
	"$OUT/render" $i 60 "$OUT/piece$i.wav"
done
echo "WAVs in $OUT/"

# the fingerprints: the sixteen at 60 s, and every audition piece at 20 s
mkdir -p "$OUT/golden"
: > "$OUT/golden.now"
for i in 0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do
	echo "stock$i $(sha1sum < "$OUT/piece$i.wav" | cut -c1-40)" >> "$OUT/golden.now"
done
i=0
while [ $i -lt 118 ]; do
	"$OUT/render_demo" $i 20 "$OUT/golden/demo.wav" > /dev/null
	echo "demo$i $(sha1sum < "$OUT/golden/demo.wav" | cut -c1-40)" >> "$OUT/golden.now"
	i=$((i + 1))
done
if [ -n "$UPDATE_GOLDEN" ] || [ ! -f golden.sha1 ]; then
	cp "$OUT/golden.now" golden.sha1
	echo "fingerprints written: host/golden.sha1 ($(wc -l < golden.sha1) renders)"
elif cmp -s "$OUT/golden.now" golden.sha1; then
	echo "fingerprints: all $(wc -l < golden.sha1) renders unchanged"
else
	echo "fingerprints CHANGED (the sound is different):"
	diff golden.sha1 "$OUT/golden.now" | grep '^>' | cut -d' ' -f2 | tr '\n' ' '
	echo
	echo "(if meant: listen, then UPDATE_GOLDEN=1 sh host/run_tests.sh)"
	exit 1
fi

# the hardware test's twin: one round of its script (glag/glag_test.h) through the controls and the
# engine; no NaN anywhere, and every scenario at 48 kHz
$CC -O2 -Wall -Wextra -Wno-unused-parameter -DGL_PROF -DGL_TEST_PIECES -DGL_TEST_HOOKS -I../glag \
	-o "$OUT/hwtest" hwtest.c ../glag/glag_ui.c ../glag/glag_engine.c -lm
"$OUT/hwtest" 1 > "$OUT/hwtest.log"
n=$(grep -c '^GLAG T3 = .*: out ' "$OUT/hwtest.log")
bad=$(grep '^GLAG T3 = .*: out ' "$OUT/hwtest.log" | grep -v 'nan 0 | 48000 Hz' || true)
if [ "$n" -lt 14 ] || [ -n "$bad" ]; then
	echo "hardware test twin: FAILED ($n scenarios)"
	echo "$bad"
	exit 1
fi
echo "hardware test twin: $n scenarios, no NaN, 48 kHz throughout ($OUT/hwtest.log)"
