#pragma once

#include "util/define.hpp"

#include <bit>
#include <cassert>
#include <fstream>
#include <sstream>
#include <iostream>

namespace cairns {

template <std::integral T>
inline void make_aligned(T& val, int64_t align) {
    assert(std::has_single_bit(static_cast<uint64_t>(align)));
    const uint64_t align_mask = static_cast<uint64_t>(align) - 1;
    val = static_cast<T>((static_cast<uint64_t>(val) + align_mask) & ~align_mask);
}

inline std::string ReadFileToString(std::string_view filepath) {
    std::ifstream fileStream(filepath.data());
    if (!fileStream.is_open()) {
        std::cerr << "Error: Could not open shader file: " << filepath << std::endl;
        return "";
    }
    std::stringstream buffer;
    buffer << fileStream.rdbuf();
    return buffer.str();
}

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

constexpr bool is_android() {
    return CAIRNS_ANDROID;
}

constexpr bool is_apple() {
    return CAIRNS_APPLE;
}

} // namespace cairns
