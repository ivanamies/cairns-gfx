// rhi/init_config.hpp
//
// Platform-handle init config for the RHI. The SDL shell (cairns_app) fills
// these in from its SDL window; the headless host (cairns_serve) sets
// `surfaceless = true` and leaves the rest zero. RHI no longer references SDL.
//
// Vulkan: shell passes the platform-specific instance extension list it needs
// (from SDL_Vulkan_GetInstanceExtensions) and a surface-creation callback (so
// RHI doesn't have to know how to turn a native window into a VkSurfaceKHR).
// The size_fn callback lets SwapChain query current pixel dims at resize
// without holding an SDL_Window* directly.
//
// Metal: shell passes a pre-resolved CA::MetalLayer*; CA::MetalLayer is opaque
// to non-Apple TUs, so it's forward-declared here. Shell uses
// SDL_Metal_CreateView + SDL_Metal_GetLayer to get it.

#pragma once

#include "util/define.hpp"

#include <cstdint>

#if CAIRNS_VULKAN
#include <vulkan/vulkan.h>
#endif

#if CAIRNS_METAL
namespace CA { class MetalLayer; }
#endif

namespace cairns::rhi {

#if CAIRNS_VULKAN
// Shell-supplied callbacks. Plain function pointers + opaque user-data to
// avoid the std::function heap allocation (this struct is on the hot path of
// resize-loop recovery).
using VkSurfaceCreateFn = bool (*)(void* user, VkInstance instance,
                                   VkSurfaceKHR* out_surface);
using WindowSizeFn = void (*)(void* user, int* w, int* h);
#endif

struct InitConfig {
    // When true, RHI initializes without any platform surface and
    // SwapChain::Init is skipped. The engine allocates an offscreen
    // final_target_ that the swap pass writes into instead.
    bool surfaceless = false;

    // Initial framebuffer dimensions (used for surfaceless final_target_
    // sizing; ignored when a real surface dictates extent).
    uint32_t width = 1280;
    uint32_t height = 720;

#if CAIRNS_VULKAN
    // Instance extensions the shell wants enabled (SDL fills this from
    // SDL_Vulkan_GetInstanceExtensions; cairns_serve passes nothing).
    const char* const* vk_instance_extensions = nullptr;
    uint32_t vk_instance_extension_count = 0;

    // Optional. Called by Device::Init after the VkInstance is created.
    // Required when surfaceless == false. Returns true on success.
    VkSurfaceCreateFn vk_create_surface = nullptr;
    void* vk_create_surface_user = nullptr;

    // Optional. Used by SwapChain to refetch pixel dims during resize.
    // Required when surfaceless == false.
    WindowSizeFn vk_window_size = nullptr;
    void* vk_window_size_user = nullptr;
#endif

#if CAIRNS_METAL
    // Pre-resolved Metal layer (shell does SDL_Metal_CreateView +
    // SDL_Metal_GetLayer). Required when surfaceless == false.
    CA::MetalLayer* metal_layer = nullptr;
#endif
};

}  // namespace cairns::rhi
