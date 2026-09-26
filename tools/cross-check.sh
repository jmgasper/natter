#!/usr/bin/env bash
# Compile-check the Haiku UI on Linux with the Haiku x86_64 cross compiler
# and Haiku's headers; the real build is native (cmake on Haiku).
set -euo pipefail
ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
CROSS=${NATTER_CROSS:-/mnt/HaikuWork/x399/build/x86_64/cross-tools-x86_64/bin/x86_64-unknown-haiku-g++}
HEADERS=${NATTER_HAIKU_HEADERS:-/mnt/HaikuWork/x399/haiku/headers}
INCLUDES=(-I"$ROOT/include" -I"$ROOT/vendor" -I"$ROOT/src/ui" -idirafter "$HEADERS/posix" -idirafter "$HEADERS" -idirafter "$HEADERS/config")
for directory in "$HEADERS"/os "$HEADERS"/os/*/ "$HEADERS"/os/add-ons/*/; do INCLUDES+=(-idirafter "$directory"); done
status=0
for source in "$ROOT"/src/ui/*.cpp; do
    echo "== $(basename "$source")"
    "$CROSS" -std=c++20 -fsyntax-only -Wall -Wextra -Wno-unused-parameter -Wno-multichar "${INCLUDES[@]}" "$source" || status=1
done
exit $status
