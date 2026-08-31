#!/bin/sh
# Host tests for the C core. No ESP-IDF toolchain and no device required.
#
# The backend tests compile the real backend sources against small stubs in
# stub/, so they check the shipped code rather than a copy of it.
set -e
CC=${CC:-cc}
SRC=breezybox-cardputer/claw
IDF=${IDF_PATH:-$HOME/esp/esp-idf}
CJSON=$IDF/components/json/cJSON
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT

echo "=== sse parser ==="
$CC -std=c11 -Wall -Wextra -Werror -I"$SRC" \
    tests/c/test_claw_sse.c "$SRC/claw_sse.c" -o "$OUT/sse"
"$OUT/sse"

echo ""
echo "=== guards ==="
$CC -std=c11 -Wall -Wextra -Werror \
    tests/c/test_claw_guard.c -o "$OUT/guard"
"$OUT/guard"

echo ""
if [ -f "$CJSON/cJSON.c" ]; then
    echo "=== text overrides ==="
    # TEXT_DIR_SD points at a scratch dir: macOS will not allow creating /sd,
    # and the end-to-end path is worth covering rather than skipping.
    $CC -std=c11 -Wall -Wextra -Werror -I"$SRC" -I tests/c/stub -I "$CJSON" \
        -DTEXT_DIR_SD="\"$OUT/claw\"" \
        tests/c/test_claw_text.c "$SRC/claw_text.c" "$SRC/claw_util.c" \
        "$CJSON/cJSON.c" -o "$OUT/text"
    "$OUT/text"

    echo ""

    echo "=== json writer ==="
    $CC -std=c11 -Wall -Wextra -Werror -I"$SRC" -I "$CJSON" \
        tests/c/test_claw_json_write.c "$SRC/claw_json_write.c" \
        "$CJSON/cJSON.c" -o "$OUT/jw"
    "$OUT/jw"

    echo ""
    echo "=== backend request bodies ==="
    $CC -std=c11 -Wall -Wextra -I"$SRC" -I tests/c/stub -I "$CJSON" \
        tests/c/test_claw_backends.c \
        "$SRC/claw_backend.c" "$SRC/claw_backend_anthropic.c" \
        "$SRC/claw_backend_openai.c" "$SRC/claw_backend_gemini.c" \
        "$CJSON/cJSON.c" -o "$OUT/backends"
    "$OUT/backends"
else
    echo "=== backend request bodies: skipped (cJSON not found at $CJSON) ==="
fi

echo ""
echo "all suites passed"
