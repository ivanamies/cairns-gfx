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

// High-resolution monotonic nanoseconds, for the profiling Timer.
uint64_t TimestampNs();

// Background worker threads for the build-draws fan-out. 0 => run inline on the
// calling thread. The browser returns 0 for now (W6a single-threaded): Web
// Worker pthreads in a WebGPU app are fragile (main-thread affinity /
// PROXY_TO_PTHREAD), so multithreading is a separate follow-up (W6b).
uint32_t WorkerThreadCount();

// Default asset base directory (trailing '/'), or empty. The CAIRNS_BASE_PATH
// env override is applied by the caller (GetBasePathSafe), not here.
std::string DefaultBasePath();

// True if the asset file exists / can be opened.
bool AssetExists(const std::filesystem::path& path);

// Read an entire asset file (text or binary) into `out`. False if unreadable.
bool ReadAsset(const std::filesystem::path& path, std::string& out);

// Per-frame imgui platform-backend hook (sets DisplaySize/DeltaTime/input from
// the windowing layer). SDL backend on native; no-op on web, where the engine's
// surfaceless path sets DisplaySize directly and DOM events feed ImGuiIO.
void ImguiNewFrame();

}  // namespace cairns::platform
