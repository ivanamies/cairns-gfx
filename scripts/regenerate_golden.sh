#!/bin/zsh
# Regenerate the byte-gate reference -> tmp/golden.png (gitignored).
#
# golden.png is backend-specific: regenerate it for the backend you intend to
# gate, at a known-good commit, then use scripts/verify_<backend>.sh to confirm
# later changes are byte-identical. Default backend is Metal (the reference).
#
# Determinism: CAIRNS_DUMP=<path> auto-engages a FixedClock (fixed-timestep sim),
# so rotation + particles are reproducible; the app dumps at sim_frame 60 and exits.
#
# Usage: scripts/regenerate_golden.sh [metal|vk]
set -e
cd "$(dirname "$0")/.."
backend="${1:-metal}"
mkdir -p tmp
out="$PWD/tmp/golden.png"
rm -f "$out"

case "$backend" in
  metal)
    [ -d build/metal/sdl-min.xcodeproj ] || cmake -G Xcode -B build/metal -DCAIRNS_GFX_BACKEND=metal -S . >/dev/null
    xcodebuild -project build/metal/sdl-min.xcodeproj -configuration Debug -scheme sdl-min build >/dev/null
    CAIRNS_N=9 CAIRNS_DUMP="$out" \
      build/metal/Debug/sdl-min.app/Contents/MacOS/sdl-min || true ;;
  vk)
    [ -d build/vk/sdl-min.xcodeproj ] || cmake -G Xcode -B build/vk -DCAIRNS_GFX_BACKEND=vulkan -S . >/dev/null
    xcodebuild -project build/vk/sdl-min.xcodeproj -configuration Debug -scheme sdl-min build >/dev/null
    icd=""
    for c in /opt/homebrew/etc/vulkan/icd.d/MoltenVK_icd.json /usr/local/share/vulkan/icd.d/MoltenVK_icd.json; do
      [ -f "$c" ] && icd="$c" && break
    done
    [ -z "$icd" ] && { echo "MoltenVK_icd.json not found; install molten-vk" >&2; exit 1; }
    VK_ICD_FILENAMES="$icd" CAIRNS_N=9 CAIRNS_DUMP="$out" \
      build/vk/Debug/sdl-min.app/Contents/MacOS/sdl-min || true ;;
  *) echo "usage: $0 [metal|vk]" >&2; exit 2 ;;
esac

[ -f "$out" ] && echo "golden ($backend) -> tmp/golden.png" || { echo "FAILED: no dump produced" >&2; exit 1; }
