#!/bin/bash
# build.sh <out dir> - builds camtune (x86_64) and copies the install files next to it
set -euo pipefail
OUT=${1:?usage: build.sh <out dir>}
HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$OUT"
xcrun clang -arch x86_64 -mmacosx-version-min=12.0 -O2 -Wall -Wextra -Wno-unused-parameter \
  -o "$OUT/camtune" "$HERE/camtune.c" -framework CoreFoundation -framework IOKit
codesign -f -s - "$OUT/camtune"
cp "$HERE/install.sh" "$HERE/uninstall.sh" "$HERE/README.md" "$OUT/"
chmod +x "$OUT/install.sh" "$OUT/uninstall.sh"
echo "camtune: $(stat -f %z "$OUT/camtune") bytes"
