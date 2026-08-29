#!/usr/bin/env bash
# Fetch the pinned wgpu-native prebuilt (see VENDORED.md). The artifacts
# (include/, lib/) are gitignored — run this once after a fresh checkout.
# Idempotent. macOS arm64 only for now (the web build uses the browser's WebGPU,
# no lib; other native platforms fetch their own asset).
set -euo pipefail
cd "$(dirname "$0")"
TAG=v29.0.0.0
ASSET=wgpu-macos-aarch64-release.zip
URL="https://github.com/gfx-rs/wgpu-native/releases/download/${TAG}/${ASSET}"
if [ -f lib/libwgpu_native.a ]; then
  echo "wgpu-native ${TAG} already present"
  exit 0
fi
echo "fetching ${URL}"
curl -sL --fail -o "${ASSET}" "${URL}"
unzip -o -q "${ASSET}"
rm -f "${ASSET}" lib/libwgpu_native.dylib   # link the static .a; drop the dylib
echo "wgpu-native ${TAG} ready: $(ls lib/)"
