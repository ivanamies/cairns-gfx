// rhi/vulkan/command_recorder_impl.hpp
//
// Internal: defines CommandRecorder::Impl for the Vulkan backend. Shared between
// vulkan/command_recorder.cpp (the method bodies) and vulkan/resource_manager.cpp
// (which constructs/destroys the Impl in BeginFrame/EndFrame). Not a public
// header — it leaks Vk* types and must only be included by Vulkan backend .cpp.

#pragma once

#include "util/define.hpp"

#if CAIRNS_VULKAN

#include <vulkan/vulkan.h>

#include "rhi/command_recorder.hpp"

namespace cairns::rhi {

struct CommandRecorder::Impl {
    Resources* res = nullptr;
    SwapChain* sc = nullptr;
    uint32_t frame = 0;
    uint32_t image_index = 0;
    VkCommandBuffer gfx = VK_NULL_HANDLE;
    VkCommandBuffer comp = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkDescriptorSet dyn_ubo_set = VK_NULL_HANDLE;
    VkDescriptorSet compute_set = VK_NULL_HANDLE;
    VkDescriptorSet point_set = VK_NULL_HANDLE;
};

}  // namespace cairns::rhi

#endif  // CAIRNS_VULKAN
