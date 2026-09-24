#!/bin/sh
# Run from any directory. Override CJSON_DIR to use another cJSON checkout.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
cjson=${CJSON_DIR:-"$root/examples/basic_thread_border_router/managed_components/espressif__cjson/cJSON"}
if [ ! -f "$cjson/cJSON.c" ]; then
    echo 'Set CJSON_DIR to a directory containing cJSON.c and cJSON.h' >&2
    exit 1
fi
binary=$(mktemp "${TMPDIR:-/tmp}/check_web_cli_sse.XXXXXX")
trap 'rm -f "$binary"' EXIT HUP INT TERM
${CC:-cc} -std=c11 -Wall -Wextra -Werror ${CFLAGS:-} \
    -I"$root/tools/ci/check_web_cli_sse_mocks" \
    -I"$root/components/esp_ot_br_server/private_include" -I"$cjson" \
    "$root/tools/ci/check_web_cli_sse.c" \
    "$root/components/esp_ot_br_server/src/esp_br_web_cli_ring.c" "$cjson/cJSON.c" \
    -lm -o "$binary"
"$binary"
