// rhi/device.hpp
//
// Platform device lifetime — the leaf of the rhi DAG. Owns the instance /
// surface / physical+logical device / queues / command pool (Vulkan) or the
// MTL::Device + command queue (Metal). Backend handles are PUBLIC members
// (SwapChain-style, no pimpl); the GIANT-CAPS banner below is the access
// contract that replaces the old friend list.

#pragma once

#include "util/define.hpp"

#if CAIRNS_VULKAN
#include <vulkan/vulkan.h>
#elif CAIRNS_METAL
#include <Metal/Metal.hpp>
#endif

struct SDL_Window;

namespace cairns::rhi {

struct SwapChain;

class Device {
public:
    Device() = default;
    ~Device();
    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;

    // CALLER: ENGINE.
    [[nodiscard]] bool Init(SDL_Window* window);
    // CALLER: ENGINE.
    void Deinit();
    // CALLER: ENGINE.
    [[nodiscard]] bool InitSwapChain(SwapChain& sc, SDL_Window* window);

    // Platform handles; the subsystems mirror these during their Init().
#if CAIRNS_VULKAN
    bool validation_enabled_ = false;
    VkInstance instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT debug_messenger_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue graphics_queue_ = VK_NULL_HANDLE;
    VkQueue compute_queue_ = VK_NULL_HANDLE;
    VkQueue present_queue_ = VK_NULL_HANDLE;
    VkCommandPool command_pool_ = VK_NULL_HANDLE;
    uint32_t queue_family_index_ = 0;
    VkSampleCountFlagBits msaa_samples_ = VK_SAMPLE_COUNT_1_BIT;
#elif CAIRNS_METAL
    MTL::Device* device_ = nullptr;
    MTL::CommandQueue* queue_ = nullptr;
#endif

private:
    bool inited_ = false;
};

}  // namespace cairns::rhi
