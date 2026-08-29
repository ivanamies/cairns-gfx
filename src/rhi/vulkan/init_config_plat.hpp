// rhi/vulkan/init_config_plat.hpp

#pragma once

#include <cstdint>

#include <vulkan/vulkan.h>

namespace cairns::rhi {

// Shell-supplied callbacks. Plain function pointers + opaque user-data to
// avoid the std::function heap allocation (this struct is on the hot path of
// resize-loop recovery).
using VkSurfaceCreateFn = bool (*)(void* user, VkInstance instance,
                                   VkSurfaceKHR* out_surface);
using WindowSizeFn = void (*)(void* user, int* w, int* h);

struct InitConfigPlat {
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
};

}  // namespace cairns::rhi
