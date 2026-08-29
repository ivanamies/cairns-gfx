// src/shell/sdl_rhi_glue_vulkan.cpp

#include "util/define.hpp"

#if CAIRNS_VULKAN

#include "shell/sdl_rhi_glue.hpp"

#include <SDL3/SDL_vulkan.h>

#include "rhi/init_config.hpp"

namespace cairns::shell {

namespace {

// Shell-side VkSurfaceKHR creation -- invoked by rhi::Device::Init after the
// VkInstance is up. |user| is the SDL_Window*.
bool ShellVkCreateSurface(void* user, VkInstance instance,
                          VkSurfaceKHR* out_surface) {
    SDL_Window* window = static_cast<SDL_Window*>(user);
    return SDL_Vulkan_CreateSurface(window, instance, nullptr, out_surface);
}

// Shell-side window pixel-size getter for SwapChain resize handling.
void ShellVkWindowSize(void* user, int* w, int* h) {
    SDL_GetWindowSizeInPixels(static_cast<SDL_Window*>(user), w, h);
}

}  // namespace

SDL_WindowFlags BackendWindowFlag() {
    return SDL_WINDOW_VULKAN;
}

const char* BackendWindowTitle() {
    return "SDL + Vulkan Sample";
}

void* AttachWindow(SDL_Window* window, rhi::InitConfig& cfg) {
    uint32_t sdl_ext_count = 0;
    const char* const* sdl_exts =
        SDL_Vulkan_GetInstanceExtensions(&sdl_ext_count);
    cfg.plat.vk_instance_extensions = sdl_exts;
    cfg.plat.vk_instance_extension_count = sdl_ext_count;
    cfg.plat.vk_create_surface = &ShellVkCreateSurface;
    cfg.plat.vk_create_surface_user = window;
    cfg.plat.vk_window_size = &ShellVkWindowSize;
    cfg.plat.vk_window_size_user = window;
    return nullptr;  // vk has no shell-side handle to release at shutdown
}

void DetachWindow(void* /*shell_handle*/) {}

}  // namespace cairns::shell

#endif  // CAIRNS_VULKAN
