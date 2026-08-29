// platform/platform_web.cpp -- Emscripten/WASM platform services via the std
// library + the preloaded MEMFS. Gated at the file level like rhi/<backend>/*.cpp:
// empty TU off the web build, where platform_sdl.cpp implements these instead.
#include "util/define.hpp"

#ifdef __EMSCRIPTEN__

#include "platform/platform.hpp"

#include <emscripten/emscripten.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
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
    if (std::filesystem::exists(path, ec) && !ec) { return true; }
    // Champion GLBs are NOT bundled into the MEMFS .data (that put ~0.9 GB
    // resident and OOM-killed the tab). They're served as loose files and
    // lazy-fetched in ReadAsset; report them as existing so the loader proceeds.
    // The fetch in ReadAsset is the real existence check (404 -> load skips it).
    return path.extension() == ".glb";
}

bool ReadAsset(const std::filesystem::path& path, std::string& out) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (f) {
        const std::streamsize n = f.tellg();
        if (n < 0) { return false; }
        f.seekg(0, std::ios::beg);
        out.resize(static_cast<size_t>(n));
        f.read(out.data(), n);
        return f.gcount() == n;
    }
    // Not in MEMFS: lazy-fetch over the network (loose champion GLB). The bytes
    // are transient -- fastgltf parses + uploads to the GPU, then `out` frees --
    // so only ~one GLB is resident at a time instead of the whole roster.
    // emscripten_wget_data is synchronous via -sASYNCIFY (the browser HTTP-caches
    // the response, so a re-load doesn't re-download). Served at the page origin.
    void* buf = nullptr;
    int size = 0;
    int err = 0;
    const std::string url = path.filename().string();
    emscripten_wget_data(url.c_str(), &buf, &size, &err);
    if (err != 0 || buf == nullptr || size <= 0) {
        if (buf) { std::free(buf); }
        return false;
    }
    out.assign(static_cast<const char*>(buf), static_cast<size_t>(size));
    std::free(buf);
    return true;
}

void ImguiNewFrame() {}  // web: DisplaySize set by the engine, input via DOM

uint32_t WorkerThreadCount() { return 0; }  // W6a: single-threaded (inline fan-out)

}  // namespace cairns::platform

#endif  // __EMSCRIPTEN__
