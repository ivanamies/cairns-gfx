// rhi/vulkan/frames_plat.hpp

#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

#include <vulkan/vulkan.h>

#include "rhi/command_recorder.hpp"  // OffscreenTargetCache + kMaxPasses + kCompositeRingSize

namespace cairns::rhi {

struct FramesPlat {
    VkDevice device_ = VK_NULL_HANDLE;          // mirrored from Device
    VkCommandPool command_pool_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkQueue graphics_queue_ = VK_NULL_HANDLE;
    VkQueue compute_queue_ = VK_NULL_HANDLE;
    VkQueue present_queue_ = VK_NULL_HANDLE;
    std::mutex swapchain_mutex_;
    std::atomic<bool> recreate_pending_{false};
    uint32_t frames_in_flight_ = 0;
    uint32_t recorder_frame_ = 0;
    std::vector<VkCommandBuffer> graphics_cmds_;
    std::vector<VkCommandBuffer> compute_cmds_;
    std::vector<VkSemaphore> image_available_;
    std::vector<VkSemaphore> render_finished_;
    std::vector<VkSemaphore> compute_finished_;
    std::vector<VkFence> in_flight_;
    std::vector<VkFence> compute_in_flight_;
    VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
    // #222 Phase F.4: descriptor set layouts moved to PipelinesPlat;
    // Frames only owns per-FIF sets + the pool below.
    std::vector<VkDescriptorSet> globals_sets_;
    std::vector<VkDescriptorSet> drawtmp_sets_;
    // Per-frame ring of composite descriptor sets for DrawFullscreen. Lets one
    // pass issue multiple fullscreen draws with distinct textures (the 997af20
    // last-bound-wins fix).
    std::vector<std::array<VkDescriptorSet, kCompositeRingSize>> composite_sets_;
};

}  // namespace cairns::rhi
