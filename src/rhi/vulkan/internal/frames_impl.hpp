// rhi/vulkan/internal/frames_impl.hpp
//
// Internal: Frames::Impl for Vulkan. Shared between vulkan/frames.cpp and
// vulkan/resource_manager.cpp (the deferred pipeline creation reads the set
// layouts via friendship). Not a public header.

#pragma once

#include "util/define.hpp"

#if CAIRNS_VULKAN

#include <cstdint>
#include <filesystem>
#include <vector>

#include <vulkan/vulkan.h>

#include "rhi/frames.hpp"

namespace cairns::rhi {

struct Frames::Impl {
    VkDevice device = VK_NULL_HANDLE;          // mirrored from Device
    VkCommandPool command_pool = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkQueue graphics_queue = VK_NULL_HANDLE;
    VkQueue compute_queue = VK_NULL_HANDLE;
    VkQueue present_queue = VK_NULL_HANDLE;
    Resources* res = nullptr;                  // borrowed

    std::filesystem::path dump_path;
    uint32_t frames_in_flight = 0;
    uint32_t recorder_frame = 0;

    std::vector<VkCommandBuffer> graphics_cmds;
    std::vector<VkCommandBuffer> compute_cmds;
    std::vector<VkSemaphore> image_available;
    std::vector<VkSemaphore> render_finished;
    std::vector<VkSemaphore> compute_finished;
    std::vector<VkFence> in_flight;
    std::vector<VkFence> compute_in_flight;

    VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
    VkDescriptorSetLayout dyn_ubo_layout = VK_NULL_HANDLE;
    VkDescriptorSetLayout compute_layout = VK_NULL_HANDLE;
    VkDescriptorSetLayout point_layout = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> dyn_ubo_sets;
    std::vector<VkDescriptorSet> compute_sets;
    std::vector<VkDescriptorSet> point_sets;
};

}  // namespace cairns::rhi

#endif  // CAIRNS_VULKAN
