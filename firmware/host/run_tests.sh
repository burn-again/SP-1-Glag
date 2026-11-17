#!/bin/sh
# Glag on a computer: the control tests, then every piece rendered and checked for level.
# usage: sh firmware/host/run_tests.sh [OUTDIR]
set -e
cd "$(dirname "$0")"
OUT=${1:-build}
mkdir -p "$OUT"
CC=${CC:-cc}
$CC -O2 -Wall -Wextra -Wdouble-promotion -I../glag -o "$OUT/ui_test" ui_test.c ../glag/glag_ui.c ../glag/glag_engine.c -lm
$CC -O2 -Wall -I../glag -o "$OUT/render" render.c ../glag/glag_engine.c -lm
"$OUT/ui_test"
for i in 0 1 2 3 4 5 6 7; do
	"$OUT/render" $i 60 "$OUT/piece$i.wav"
done
echo "WAVs in $OUT/"
