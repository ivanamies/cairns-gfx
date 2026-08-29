// rhi/vulkan/internal/device_impl.hpp
//
// Internal: defines Device::Impl for the Vulkan backend. Shared between
// vulkan/device.cpp (creates/destroys) and vulkan/resource_manager.cpp (mirrors
// the handle values into its own Impl during InitDevice). Not a public header.

#pragma once

#include "util/define.hpp"

#if CAIRNS_VULKAN

#include <cstdint>

#include <vulkan/vulkan.h>

#include "rhi/device.hpp"

namespace cairns::rhi {

struct Device::Impl {
    bool validation_enabled = false;
    VkInstance instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT debug_messenger = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue graphics_queue = VK_NULL_HANDLE;
    VkQueue compute_queue = VK_NULL_HANDLE;
    VkQueue present_queue = VK_NULL_HANDLE;
    VkCommandPool command_pool = VK_NULL_HANDLE;
    uint32_t queue_family_index = 0;
    VkSampleCountFlagBits msaa_samples = VK_SAMPLE_COUNT_1_BIT;
};

}  // namespace cairns::rhi

#endif  // CAIRNS_VULKAN
