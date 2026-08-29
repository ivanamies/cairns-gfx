#pragma once

#include "util/define.hpp"

#include <filesystem>
#include <string_view>

#include <SDL3/SDL.h>

namespace cairns {

inline bool GetStaticResourceFilepath(std::string_view file, std::filesystem::path& output) {
#if CAIRNS_ANDROID
    // SDL3 looks up relative paths via the APK AssetManager -- same place
    // CMake's copy_helper installs to. No adb push step, no external storage
    // detour. Same way shader loading already worked.
    output = std::filesystem::path(file);
#elif CAIRNS_APPLE
    const char* basePathPtr = SDL_GetBasePath();
    if (not basePathPtr){
        return false;
    }
    const std::filesystem::path basePath = basePathPtr;
    output = basePath / file;
#endif

    SDL_IOStream* io = SDL_IOFromFile(output.string().c_str(), "rb");
    if (!io) {
        return false;
    }
    SDL_CloseIO(io);
    return true;
}

} // namespace cairns
