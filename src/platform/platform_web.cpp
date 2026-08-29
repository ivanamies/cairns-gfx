// platform/platform_web.cpp -- Emscripten/WASM platform services via the std
// library + the preloaded MEMFS. Gated at the file level like rhi/<backend>/*.cpp:
// empty TU off the web build, where platform_sdl.cpp implements these instead.
#include "util/define.hpp"

#ifdef __EMSCRIPTEN__

#include "platform/platform.hpp"

#include <emscripten/emscripten.h>

#include <SDL3/SDL.h>

#include "imgui.h"
#include "imgui_impl_sdl3.h"

#include <cstdio>
#include <cstdlib>

namespace cairns::platform {

uint64_t TicksMs() { return SDL_GetTicks(); }

uint64_t TimestampNs() { return SDL_GetTicksNS(); }

std::string DefaultBasePath() {
    return "/";  // assets preloaded at the MEMFS root (--preload-file assets@/)
}

bool AssetExists(const std::filesystem::path& path) {
    // MEMFS lookup via SDL (POSIX-backed on emscripten, same path as native).
    SDL_IOStream* io = SDL_IOFromFile(path.string().c_str(), "rb");
    if (io) {
        SDL_CloseIO(io);
        return true;
    }
    // Champion GLBs are NOT bundled into the MEMFS .data (that put ~0.9 GB
    // resident and OOM-killed the tab). They're served as loose files and
    // lazy-fetched in ReadAsset; report them as existing so the loader proceeds.
    // The fetch in ReadAsset is the real existence check (404 -> load skips it).
    return path.extension() == ".glb";
}

bool ReadAsset(const std::filesystem::path& path, std::string& out) {
    // MEMFS read via SDL (POSIX-backed); the loose champion GLBs miss here and
    // fall through to the network fetch below.
    size_t n = 0;
    void* data = SDL_LoadFile(path.string().c_str(), &n);
    if (data) {
        out.assign(static_cast<const char*>(data), n);
        SDL_free(data);
        return true;
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

void ImguiNewFrame() { ImGui_ImplSDL3_NewFrame(); }  // SDL drives DisplaySize + input

uint32_t WorkerThreadCount() { return 0; }  // single-threaded (inline fan-out)

}  // namespace cairns::platform

#endif  // __EMSCRIPTEN__
