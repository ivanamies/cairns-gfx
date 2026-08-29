// platform/platform.hpp
//
// Opaque platform-services seam. Declarations only -- no SDL, no <chrono>, no
// platform headers leak to callers, so platform-independent code (util/,
// control/, engine) stays free of platform #ifdefs. Exactly one implementation
// translation unit is live per build, each gated like the rhi/<backend>/ files:
//   platform_sdl.cpp -- native desktop/mobile (SDL)
//   platform_web.cpp -- Emscripten/WASM (std + browser)
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace cairns::platform {

// Monotonic milliseconds, for WallClock's per-frame delta.
uint64_t TicksMs();

// Default asset base directory (trailing '/'), or empty. The CAIRNS_BASE_PATH
// env override is applied by the caller (GetBasePathSafe), not here.
std::string DefaultBasePath();

// True if the asset file exists / can be opened.
bool AssetExists(const std::filesystem::path& path);

// Read an entire asset file (text or binary) into `out`. False if unreadable.
bool ReadAsset(const std::filesystem::path& path, std::string& out);

}  // namespace cairns::platform
