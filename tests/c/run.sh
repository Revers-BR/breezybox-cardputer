#!/bin/sh
# Host tests for the C core. No ESP-IDF required.
set -e
CC=${CC:-cc}
SRC=breezybox-cardputer/claw
OUT=$(mktemp -d)
$CC -std=c11 -Wall -Wextra -Werror -I"$SRC" \
    tests/c/test_claw_sse.c "$SRC/claw_sse.c" -o "$OUT/test_claw_sse"
"$OUT/test_claw_sse"
rm -rf "$OUT"
