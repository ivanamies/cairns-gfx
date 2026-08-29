// platform/platform_web.cpp -- Emscripten/WASM platform services via the std
// library + the preloaded MEMFS. Gated at the file level like rhi/<backend>/*.cpp:
// empty TU off the web build, where platform_sdl.cpp implements these instead.
#include "util/define.hpp"

#ifdef __EMSCRIPTEN__

#include "platform/platform.hpp"

#include <chrono>
#include <cstdio>
#include <fstream>

namespace cairns::platform {

uint64_t TicksMs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

uint64_t TimestampNs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
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
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) { return false; }
    const std::streamsize n = f.tellg();
    if (n < 0) { return false; }
    f.seekg(0, std::ios::beg);
    out.resize(static_cast<size_t>(n));
    f.read(out.data(), n);
    return f.gcount() == n;
}

void ImguiNewFrame() {}  // web: DisplaySize set by the engine, input via DOM

uint32_t WorkerThreadCount() { return 0; }  // W6a: single-threaded (inline fan-out)

}  // namespace cairns::platform

#endif  // __EMSCRIPTEN__
