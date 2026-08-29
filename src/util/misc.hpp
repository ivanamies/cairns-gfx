#pragma once

#include "util/define.hpp"
#include "platform/platform.hpp"

#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>

namespace cairns {

// Single source of the "where do assets live" answer. CAIRNS_BASE_PATH env
// var wins (used by raw-binary Android tests that have no SDL Activity to
// resolve nativeLibraryDir from); else the platform default (SDL_GetBasePath
// on Apple, MEMFS root on web, empty for Android APK / cwd).
inline std::string GetBasePathSafe() {
    if (const char* env = std::getenv("CAIRNS_BASE_PATH")) {
        std::string s = env;
        if (!s.empty() && s.back() != '/') {
            s.push_back('/');
        }
        return s;
    }
    return platform::DefaultBasePath();
}

inline bool GetStaticResourceFilepath(std::string_view file, std::filesystem::path& output) {
    const std::string base = GetBasePathSafe();
    output = base.empty() ? std::filesystem::path(file)
                          : std::filesystem::path(base) / file;

    // Android raw-binary tests (no SDL Activity) segfault inside the SDL
    // asset open -- bypass via plain stdlib when CAIRNS_BASE_PATH is set
    // (the test harness opts in explicitly).
    if (std::getenv("CAIRNS_BASE_PATH")) {
        std::error_code ec;
        return std::filesystem::exists(output, ec) && !ec;
    }

    return platform::AssetExists(output);
}

} // namespace cairns
