// platform/platform_sdl.cpp -- native (desktop/mobile) platform services via
// SDL. Gated at the file level like rhi/<backend>/*.cpp: empty TU on the web
// build, where platform_web.cpp provides the implementation instead.
#include "util/define.hpp"

#ifndef __EMSCRIPTEN__

#include "platform/platform.hpp"

#include <SDL3/SDL.h>

namespace cairns::platform {

uint64_t TicksMs() { return SDL_GetTicks(); }

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

}  // namespace cairns::platform

#endif  // !__EMSCRIPTEN__
