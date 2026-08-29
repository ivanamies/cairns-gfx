#!/bin/zsh
# Byte-gate the Metal build against tmp/golden.png.
# Regenerate golden from the SAME backend at a known-good commit first:
#   scripts/regenerate_golden.sh metal
set -e
cd "$(dirname "$0")/.."
mkdir -p tmp
cur="$PWD/tmp/last_metal.png"
rm -f "$cur"

[ -d build/metal/sdl-min.xcodeproj ] || cmake -G Xcode -B build/metal -DCAIRNS_GFX_BACKEND=metal -S . >/dev/null
xcodebuild -project build/metal/sdl-min.xcodeproj -configuration Debug -scheme sdl-min build >/dev/null
CAIRNS_FREEZE_ROT=45 CAIRNS_DUMP="$cur" \
  build/metal/Debug/sdl-min.app/Contents/MacOS/sdl-min || true

[ -f "$cur" ] || { echo "METAL: no frame dumped" >&2; exit 1; }
[ -f tmp/golden.png ] || { echo "METAL: tmp/golden.png missing (run scripts/regenerate_golden.sh metal)" >&2; exit 1; }
if cmp -s "$cur" tmp/golden.png; then
  echo "METAL: byte-identical to golden ✓"
else
  echo "METAL: DIFFERS from golden ✗  (tmp/last_metal.png vs tmp/golden.png)" >&2
  exit 1
fi
