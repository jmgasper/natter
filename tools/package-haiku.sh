#!/bin/bash
# Make natter-<version>-1-<arch>.hpkg on Haiku from a built tree:
#   cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build --target Natter
#   tools/package-haiku.sh [build directory] [output directory]
# Installs apps/Natter with a Deskbar entry. A build with -DNATTER_WEBKIT
# (sign-in on Slack's web page) also requires Summit's engine.
set -euo pipefail
ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
BUILD=$(cd -- "${1:-$ROOT/build}" && pwd)
OUT=${2:-$BUILD}
VERSION=$(sed -n 's/^project(natter VERSION \([0-9.]*\).*/\1/p' "$ROOT/CMakeLists.txt")
ARCH=$(uname -m)
STAGE=$(mktemp -d "${TMPDIR:-/tmp}/natter-package.XXXXXX")
trap 'rm -rf "$STAGE"' EXIT

mkdir -p "$STAGE/apps/Natter" "$STAGE/data/deskbar/menu/Applications" "$STAGE/documentation/packages/natter"
# strip drops the resources appended to the executable: add them back.
cp "$BUILD/Natter" "$STAGE/apps/Natter/Natter"
strip --strip-debug "$STAGE/apps/Natter/Natter"
xres -o "$STAGE/apps/Natter/Natter" "$BUILD/Natter.rsrc"
mimeset -f "$STAGE/apps/Natter/Natter"
ln -s ../../../../apps/Natter/Natter "$STAGE/data/deskbar/menu/Applications/Natter"
cp "$ROOT/README.md" "$ROOT/LICENSE" "$STAGE/documentation/packages/natter/"

WEBKIT=""
if readelf -d "$STAGE/apps/Natter/Natter" | grep -q "libWebKit"; then
	WEBKIT="	lib:libWebKit"
fi
cat > "$STAGE/.PackageInfo" <<INFO
name			natter
version			$VERSION-1
architecture	$ARCH
summary			"A native Slack client"
description		"Natter is a Slack client written for Haiku: channels and direct \
messages with unread counts, threads, reactions, files and images, mentions, \
custom emoji, editing, uploads and notifications, updated in real time. It \
signs in with a Slack session (the web client's cookie) or your own Slack app's tokens."
packager		"KunanyiOS contributors"
vendor			"KunanyiOS"
copyrights		{ "2026 KunanyiOS contributors" }
licenses		{ "MIT" }
provides {
	natter = $VERSION
	app:Natter = $VERSION
}
requires {
	haiku
	lib:libcurl >= 4
	lib:libssl >= 3
	lib:libcrypto >= 3
	lib:libgcc_s
	lib:libstdc++
	noto_emoji
$WEBKIT
}
INFO
mkdir -p "$OUT"
package create -C "$STAGE" "$OUT/natter-$VERSION-1-$ARCH.hpkg"
echo "$OUT/natter-$VERSION-1-$ARCH.hpkg"
