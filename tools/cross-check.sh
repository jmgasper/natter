#!/usr/bin/env bash
# Compile-check the Haiku UI on Linux with the Haiku x86_64 cross compiler
# and Haiku's headers; the real build is native (cmake on Haiku).
set -euo pipefail
ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
CROSS=${NATTER_CROSS:-/mnt/HaikuWork/x399/build/x86_64/cross-tools-x86_64/bin/x86_64-unknown-haiku-g++}
HEADERS=${NATTER_HAIKU_HEADERS:-/mnt/HaikuWork/x399/haiku/headers}
WEBKIT=${NATTER_WEBKIT_SOURCE:-/mnt/HaikuWork/apps/summit/.cache/WebKit/Source/WebKit}
# Summit's engine headers, laid out as an installed engine has them, for the
# browser sign-in (NATTER_WEBKIT).
STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT
mkdir -p "$STAGE/WebKit"
for header in UIProcess/API/haiku/WebKitView.h UIProcess/API/haiku/WebKitContext.h \
    UIProcess/API/haiku/WebKitExtensionPermission.h UIProcess/API/haiku/WebKitInfo.h \
    UIProcess/API/haiku/WebKitEmbedding.h Shared/API/c/WKBase.h Shared/API/c/WKDeclarationSpecifiers.h \
    Shared/API/c/haiku/WKBaseHaiku.h; do
    [[ -f $WEBKIT/$header ]] && cp "$WEBKIT/$header" "$STAGE/WebKit/"
done
INCLUDES=(-I"$ROOT/include" -I"$ROOT/vendor" -I"$ROOT/src/ui" -I"$STAGE" -idirafter "$HEADERS/posix" -idirafter "$HEADERS" -idirafter "$HEADERS/config")
for directory in "$HEADERS"/os "$HEADERS"/os/*/ "$HEADERS"/os/add-ons/*/; do INCLUDES+=(-idirafter "$directory"); done
status=0
for source in "$ROOT"/src/ui/*.cpp; do
    echo "== $(basename "$source")"
    for webkit in 0 1; do
        [[ $webkit == 0 && $source == */BrowserSignInWindow.cpp ]] && continue
        [[ $webkit == 1 ]] && ! grep -q "NATTER_WEBKIT\|BrowserSignIn" "$source" && continue
        "$CROSS" -std=c++20 -fsyntax-only -Wall -Wextra -Wno-unused-parameter -Wno-multichar \
            -DNATTER_WEBKIT=$webkit "${INCLUDES[@]}" "$source" || status=1
    done
done
exit $status
