#pragma once

#include "util/define.hpp"

#include <filesystem>
#include <string_view>

#include <SDL3/SDL.h>

namespace cairns {

inline bool GetStaticResourceFilepath(std::string_view file, std::filesystem::path& output) {
#if CAIRNS_ANDROID
    // On Android, large assets (GLBs) are adb-pushed to the app's external files dir.
    const char* basePathPtr = SDL_GetAndroidExternalStoragePath();
    if (not basePathPtr){
        return false;
    }
    const std::filesystem::path basePath = basePathPtr;
#elif CAIRNS_APPLE
    const char* basePathPtr = SDL_GetBasePath();
    if (not basePathPtr){
        return false;
    }
    const std::filesystem::path basePath = basePathPtr;
#endif // CAIRNS_APPLE

    output = basePath / file;
    SDL_IOStream* io = SDL_IOFromFile(output.string().c_str(), "rb");
    if (!io) {
        return false;
    }
    SDL_CloseIO(io);
    return true;
}

} // namespace cairns
