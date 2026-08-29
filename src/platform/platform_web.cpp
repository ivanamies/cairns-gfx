// platform/platform_web.cpp -- Emscripten/WASM platform services via the std
// library + the preloaded MEMFS. Gated at the file level like rhi/<backend>/*.cpp:
// empty TU off the web build, where platform_sdl.cpp implements these instead.
#include "util/define.hpp"

#ifdef __EMSCRIPTEN__

#include "platform/platform.hpp"

#include <chrono>
#include <fstream>
#include <sstream>

namespace cairns::platform {

uint64_t TicksMs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

std::string DefaultBasePath() {
    return "/";  // assets preloaded at the MEMFS root (--preload-file assets@/)
}

bool AssetExists(const std::filesystem::path& path) {
    std::error_code ec;
    return std::filesystem::exists(path, ec) && !ec;
}

bool ReadAsset(const std::filesystem::path& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { return false; }
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

}  // namespace cairns::platform

#endif  // __EMSCRIPTEN__
