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
