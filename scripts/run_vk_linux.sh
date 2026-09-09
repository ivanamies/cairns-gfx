#!/usr/bin/env bash
# Linux Vulkan launcher: fix up the container environment, pick an ICD, exec.
#
# Configure + build first (clang; the -Wall/-Wextra/-Werror set is calibrated
# for clang, and gcc's extra diagnostics fail the build):
#
#   cmake -B build/vk-linux -G Ninja -DCMAKE_BUILD_TYPE=Release \
#         -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
#         -DCAIRNS_GFX_BACKEND=vulkan \
#         -DCAIRNS_GFX_BUILD_TESTS=ON -DCAIRNS_GFX_BUILD_GOLDEN_TESTS=ON \
#         -DSDL_X11_XSCRNSAVER=OFF -DSDL_X11_XTEST=OFF
#   cmake --build build/vk-linux -j
#
# Usage:  scripts/run_vk_linux.sh <binary-name> [args...]
#   e.g.  scripts/run_vk_linux.sh cairns_golden_tests "[subject]"
#         RUN_VK_ICD=lvp scripts/run_vk_linux.sh cairns_serve < drive.ndjson
#
# Env:
#   CAIRNS_BUILD_DIR  build tree (default build/vk-linux)
#   RUN_VK_ICD        nvidia | lvp | <path> | auto (default auto)
set -euo pipefail
cd "$(dirname "$0")/.."

# 1. DISPLAY. Even a surfaceless render needs a reachable X server: SDL_GPU
#    selects its backend through the video driver, and SDL_VIDEODRIVER=dummy
#    yields "No supported SDL_GPU backend found". The container's baked-in
#    DISPLAY is the *host's* value and usually points at a socket that was
#    never mounted, so derive it from whichever socket is actually there.
if [ -z "${DISPLAY:-}" ] || [ ! -e "/tmp/.X11-unix/X${DISPLAY##*:}" ]; then
    sock=$(ls /tmp/.X11-unix/X* 2>/dev/null | head -1 || true)
    if [ -n "$sock" ]; then
        export DISPLAY=":${sock##*/X}"
    else
        echo "run_vk_linux: no X socket in /tmp/.X11-unix" >&2
    fi
fi

# 2. XDG_RUNTIME_DIR is unset in the container; Mesa complains on every call.
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/tmp/runtime-$(id -u)}"
mkdir -p "$XDG_RUNTIME_DIR"
chmod 700 "$XDG_RUNTIME_DIR"

# 3. ICD. The image bakes VK_ICD_FILENAMES=lavapipe into every shell, so "it is
#    already set" cannot mean "the user chose it" -- always pin it here.
NV_ICD=/etc/vulkan/icd.d/nvidia_icd.json
LVP_ICD=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json
case "${RUN_VK_ICD:-auto}" in
    nvidia) ICD="$NV_ICD" ;;
    lvp)    ICD="$LVP_ICD" ;;
    auto)   if [ -e /dev/nvidia0 ] && [ -f "$NV_ICD" ]; then ICD="$NV_ICD"; else ICD="$LVP_ICD"; fi ;;
    *)      ICD="$RUN_VK_ICD" ;;
esac
[ -f "$ICD" ] || { echo "run_vk_linux: ICD not found at $ICD" >&2; exit 1; }
export VK_ICD_FILENAMES="$ICD"

BUILD_DIR="${CAIRNS_BUILD_DIR:-build/vk-linux}"
[ $# -ge 1 ] || { echo "usage: $0 <binary-name> [args...]" >&2; exit 2; }
BIN="$BUILD_DIR/Release/$1"
shift
[ -x "$BIN" ] || { echo "run_vk_linux: $BIN not built" >&2; exit 1; }
echo "run_vk_linux: DISPLAY=$DISPLAY ICD=$(basename "$ICD") -> $BIN" >&2
exec "$BIN" "$@"
