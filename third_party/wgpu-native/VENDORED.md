# wgpu-native (vendored binary)

gfx-rs/wgpu-native — the C bindings for wgpu (Rust), consumed as a **prebuilt
binary**. No Rust toolchain, no submodule build.

> The binary artifacts (`include/`, `lib/`) are **gitignored** — run `./fetch.sh`
> once after a fresh checkout to download the pinned release.

- **Tag:** `v29.0.0.0`
- **Asset:** `wgpu-macos-aarch64-release.zip`
  (`https://github.com/gfx-rs/wgpu-native/releases/download/v29.0.0.0/wgpu-macos-aarch64-release.zip`)
- **Vendored:** 2026-06-22

## Contents

- `include/webgpu/webgpu.h` — the standard webgpu.h (future/callback-info API,
  `WGPUStringView`).
- `include/webgpu/wgpu.h` — wgpu-native-only extensions.
- `lib/libwgpu_native.a` — static lib (link this).
- `lib/libwgpu_native.dylib` — dynamic lib (unused).
- `wgpu-native-meta/` — upstream metadata.

## macOS link deps (the static lib pulls these)

`Metal QuartzCore Foundation IOKit IOSurface CoreFoundation Security CoreGraphics`
(+ libc++). wgpu-native runs over Metal on macOS.

## Updating

Bump the tag, re-download the matching `wgpu-macos-aarch64-release.zip`, replace
`include/` + `lib/`. `webgpu.h` has breaking changes across versions — re-verify
the backend compiles. Other platforms (linux/windows/ios/android/wasm) pull their
own asset; the web build uses the browser's WebGPU (no lib).
