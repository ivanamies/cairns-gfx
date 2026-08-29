// rhi/vulkan/internal/resources_impl.hpp
//
// Internal: Resources::Impl for Vulkan. Shared between vulkan/resources.cpp and
// vulkan/resource_manager.cpp (transitional: RM borrows Resources* and routes
// pool access through it). Not a public header.

#pragma once

#include "util/define.hpp"

#if CAIRNS_VULKAN

#include <cstdint>

#include <vulkan/vulkan.h>

#include "rhi/resources.hpp"

namespace cairns::rhi {

struct Resources::Impl {
    VkDevice device = VK_NULL_HANDLE;          // mirrored from Device
    VkCommandPool command_pool = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    Allocator* alloc = nullptr;                // borrowed
    uint32_t frame_index = 0;                  // drives deferred-free + bump retire
};

}  // namespace cairns::rhi

#endif  // CAIRNS_VULKAN
