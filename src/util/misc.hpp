#pragma once

#include "util/define.hpp"

#include <filesystem>
#include <string_view>

namespace cairns {

inline bool GetStaticResourceFilepath(std::string_view file, std::filesystem::path& output) {
#if CAIRNS_ANDROID
    std::filesystem::path basePath = "";   // on Android we do not want to use basepath. Instead, assets are available at the root directory.
#elif CAIRNS_APPLE
    auto basePathPtr = SDL_GetBasePath();
    if (not basePathPtr){
        return false;
    }
    const std::filesystem::path basePath = basePathPtr;
#endif // CAIRNS_APPLE
    
    output = basePath / file;
    return std::filesystem::exists(output);
}

} // namespace cairns
