#!/bin/zsh
# Run the Vulkan build via MoltenVK, overriding any global VK_ICD_FILENAMES
# (e.g. a mock_icd left set by vulkan-tools) that would otherwise pick a
# non-rendering driver. Configure/build first: cmake -B build/vk -G Xcode
# -DCAIRNS_GFX_BACKEND=vulkan && xcodebuild -project build/vk/sdl-min.xcodeproj
# -configuration Debug -scheme sdl-min build
set -e
cd "$(dirname "$0")"

ICD=""
for cand in \
  /opt/homebrew/etc/vulkan/icd.d/MoltenVK_icd.json \
  /usr/local/share/vulkan/icd.d/MoltenVK_icd.json; do
  [ -f "$cand" ] && ICD="$cand" && break
done
if [ -z "$ICD" ]; then
  echo "MoltenVK_icd.json not found; install molten-vk" >&2
  exit 1
fi
echo "using VK_ICD_FILENAMES=$ICD"
exec env VK_ICD_FILENAMES="$ICD" \
  build/vk/Debug/sdl-min.app/Contents/MacOS/sdl-min "$@"
