#pragma once

#include "util/define.hpp"

#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>

#ifndef __EMSCRIPTEN__
#include <SDL3/SDL.h>  // no SDL in the web build; assets resolve via MEMFS
#endif

namespace cairns {

// Single source of the "where do assets live" answer. CAIRNS_BASE_PATH env
// var wins (used by raw-binary Android tests that have no SDL Activity to
// resolve nativeLibraryDir from); else SDL_GetBasePath on Apple; else empty
// (Android APK resolves via AssetManager from cwd).
inline std::string GetBasePathSafe() {
    if (const char* env = std::getenv("CAIRNS_BASE_PATH")) {
        std::string s = env;
        if (!s.empty() && s.back() != '/') {
            s.push_back('/');
        }
        return s;
    }
#if defined(__EMSCRIPTEN__)
    return "/";  // assets preloaded at the MEMFS root (--preload-file assets@/)
#elif CAIRNS_APPLE
    const char* p = SDL_GetBasePath();
    return p ? std::string(p) : std::string();
#else
    return {};
#endif
}

inline bool GetStaticResourceFilepath(std::string_view file, std::filesystem::path& output) {
    const std::string base = GetBasePathSafe();
    output = base.empty() ? std::filesystem::path(file)
                          : std::filesystem::path(base) / file;

#if defined(__EMSCRIPTEN__)
    // Web: assets live in the preloaded MEMFS; resolve via plain stdlib (no SDL).
    std::error_code ec;
    return std::filesystem::exists(output, ec) && !ec;
#else
    // Android raw-binary tests (no SDL Activity) segfault inside
    // SDL_IOFromFile -- bypass via plain stdlib when CAIRNS_BASE_PATH is set
    // (the test harness opts in explicitly).
    if (std::getenv("CAIRNS_BASE_PATH")) {
        std::error_code ec;
        return std::filesystem::exists(output, ec) && !ec;
    }

    SDL_IOStream* io = SDL_IOFromFile(output.string().c_str(), "rb");
    if (!io) {
        return false;
    }
    SDL_CloseIO(io);
    return true;
#endif
}

} // namespace cairns
