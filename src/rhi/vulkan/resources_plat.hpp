// rhi/vulkan/resources_plat.hpp

#pragma once

#include <cstdint>

#include <vulkan/vulkan.h>

namespace cairns::rhi {

struct ResourcesPlat {
    VkDevice device_ = VK_NULL_HANDLE;          // mirrored from Device
    VkCommandPool command_pool_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    uint32_t frame_index_ = 0;                  // drives deferred-free + bump retire
    VkDescriptorSetLayout material_set_layout_ = VK_NULL_HANDLE;  // set 2 (lazy)
    VkDescriptorPool material_pool_ = VK_NULL_HANDLE;             // per-material sets
};

}  // namespace cairns::rhi
