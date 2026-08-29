// platform/platform_sdl.cpp -- native (desktop/mobile) platform services via
// SDL. Gated at the file level like rhi/<backend>/*.cpp: empty TU on the web
// build, where platform_web.cpp provides the implementation instead.
#include "util/define.hpp"

#ifndef __EMSCRIPTEN__

#include "platform/platform.hpp"

#include <SDL3/SDL.h>

#include <thread>

#include "imgui.h"
#include "imgui_impl_sdl3.h"

namespace cairns::platform {

uint64_t TicksMs() { return SDL_GetTicks(); }

uint64_t TimestampNs() {
    // Apple Silicon / ARM generic timer: count (cntvct_el0) scaled by freq.
    uint64_t freq;
    asm volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    uint64_t count;
    asm volatile("isb" : : : "memory");
    asm volatile("mrs %0, cntvct_el0" : "=r"(count));
    if (freq == 1'000'000'000ull) { return count; }
    return static_cast<uint64_t>(
        (static_cast<__uint128_t>(count) * 1'000'000'000ull) / freq);
}

std::string DefaultBasePath() {
#if CAIRNS_APPLE
    const char* p = SDL_GetBasePath();
    return p ? std::string(p) : std::string();
#else
    return {};  // Android resolves via AssetManager from cwd
#endif
}

bool AssetExists(const std::filesystem::path& path) {
    // SDL_IOFromFile resolves APK assets on Android (std::filesystem can't).
    SDL_IOStream* io = SDL_IOFromFile(path.string().c_str(), "rb");
    if (!io) { return false; }
    SDL_CloseIO(io);
    return true;
}

bool ReadAsset(const std::filesystem::path& path, std::string& out) {
    size_t n = 0;
    void* data = SDL_LoadFile(path.string().c_str(), &n);
    if (!data) { return false; }
    out.assign(static_cast<const char*>(data), n);
    SDL_free(data);
    return true;
}

void ImguiNewFrame() { ImGui_ImplSDL3_NewFrame(); }

uint32_t WorkerThreadCount() {
    if (std::getenv("CAIRNS_SINGLE_THREAD")) { return 0; }  // diag/web-parity
    // Cap at 4 so M-series fan-out stays on P-cores (hardware_concurrency()
    // counts E-cores too, which ate the win); clamp 0 -> 1.
    const unsigned hw = std::thread::hardware_concurrency();
    const unsigned n = hw > 0 ? hw : 1u;
    return n > 4u ? 4u : n;
}

}  // namespace cairns::platform

#endif  // !__EMSCRIPTEN__
