#!/bin/sh
# Host unit tests for the BLE parsers, history and fan logic (no ESP-IDF needed).
# fan.c includes "board.h" with quotes, so it is copied next to a stub board.h.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
MAIN="$HERE/../../main"
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT

CFLAGS="-w -g -fsanitize=address,undefined -I $HERE/stub -I $MAIN"

gcc $CFLAGS "$HERE/test_sensors.c" -o "$OUT/test_sensors"
"$OUT/test_sensors" | tail -1

cp "$MAIN/fan.c" "$OUT/fan.c"
cp "$HERE/stub_board.h" "$OUT/board.h"
cp "$HERE/test_fan.c" "$OUT/test_fan.c"
gcc $CFLAGS "$OUT/test_fan.c" -o "$OUT/test_fan"
"$OUT/test_fan" | tail -1
