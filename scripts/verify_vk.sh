#!/bin/zsh
# Byte-gate the Vulkan (MoltenVK) build against tmp/golden.png.
# Regenerate golden from the SAME backend at a known-good commit first:
#   scripts/regenerate_golden.sh vk
# (MoltenVK output is not byte-identical to native Metal, so gate vk against a
#  vk-regenerated golden, not a Metal one.)
set -e
cd "$(dirname "$0")/.."
mkdir -p tmp
cur="$PWD/tmp/last_vk.png"
rm -f "$cur"

[ -d build/vk/sdl-min.xcodeproj ] || cmake -G Xcode -B build/vk -DCAIRNS_GFX_BACKEND=vulkan -S . >/dev/null
xcodebuild -project build/vk/sdl-min.xcodeproj -configuration Debug -scheme sdl-min build >/dev/null

icd=""
for c in /opt/homebrew/etc/vulkan/icd.d/MoltenVK_icd.json /usr/local/share/vulkan/icd.d/MoltenVK_icd.json; do
  [ -f "$c" ] && icd="$c" && break
done
[ -z "$icd" ] && { echo "MoltenVK_icd.json not found; install molten-vk" >&2; exit 1; }

VK_ICD_FILENAMES="$icd" CAIRNS_N=9 CAIRNS_DUMP="$cur" \
  build/vk/Debug/sdl-min.app/Contents/MacOS/sdl-min || true

[ -f "$cur" ] || { echo "VK: no frame dumped" >&2; exit 1; }
[ -f tmp/golden.png ] || { echo "VK: tmp/golden.png missing (run scripts/regenerate_golden.sh vk)" >&2; exit 1; }
if cmp -s "$cur" tmp/golden.png; then
  echo "VK: byte-identical to golden ✓"
else
  echo "VK: DIFFERS from golden ✗  (tmp/last_vk.png vs tmp/golden.png)" >&2
  exit 1
fi
