// src/shell/sdl_rhi_glue_metal.cpp

#include "util/define.hpp"

#if CAIRNS_METAL

#include "shell/sdl_rhi_glue.hpp"

#include <SDL3/SDL_metal.h>

#include "rhi/init_config.hpp"

namespace cairns::shell {

SDL_WindowFlags BackendWindowFlag() {
    return SDL_WINDOW_METAL;
}

const char* BackendWindowTitle() {
    return "SDL + Metal-cpp Sample";
}

void* AttachWindow(SDL_Window* window, rhi::InitConfig& cfg) {
    SDL_MetalView view = SDL_Metal_CreateView(window);
    if (!view) {
        return nullptr;
    }
    cfg.plat.metal_layer =
        static_cast<CA::MetalLayer*>(SDL_Metal_GetLayer(view));
    if (!cfg.plat.metal_layer) {
        SDL_Metal_DestroyView(view);
        return nullptr;
    }
    return view;
}

void DetachWindow(void* shell_handle) {
    if (shell_handle) {
        SDL_Metal_DestroyView(static_cast<SDL_MetalView>(shell_handle));
    }
}

}  // namespace cairns::shell

#endif  // CAIRNS_METAL
