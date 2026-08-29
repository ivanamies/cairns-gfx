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
    VkDescriptorSetLayout globals_set_layout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout drawtmp_set_layout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout compute_layout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout point_layout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout composite_set_layout_ = VK_NULL_HANDLE;
    // #221 Skinning Phase 4: two layouts for the skin kernel. Group B is
    // frame-global (dynUBO Params @ 0, dynSSBO palettes @ 1, dynSSBO
    // InstanceMeta @ 2, SSBO output pool whole @ 3). Group A is per-mesh
    // (SSBO positions slice @ 0, SSBO skin-attrs slice @ 1). Group B sets
    // are allocated per frame-in-flight; Group A sets are allocated at
    // load via Resources::CreateBindGroup and stored on Mesh::Hot.
    VkDescriptorSetLayout skin_group_b_layout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout skin_group_a_layout_ = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> skin_group_b_sets_;
    std::vector<VkDescriptorSet> globals_sets_;
    std::vector<VkDescriptorSet> drawtmp_sets_;
    // One DescriptorSet per in-flight slot per sim step. Indexed
    // [frame_in_flight][step_index]. Multi-step compute needs distinct sets
    // because vkUpdateDescriptorSets on an in-use set is UB.
    std::vector<std::array<VkDescriptorSet, kMaxStepsPerFrame>> compute_sets_;
    std::vector<VkDescriptorSet> point_sets_;
    // Per-frame ring of composite descriptor sets for DrawFullscreen. Lets one
    // pass issue multiple fullscreen draws with distinct textures (the 997af20
    // last-bound-wins fix).
    std::vector<std::array<VkDescriptorSet, kCompositeRingSize>> composite_sets_;
    OffscreenTargetCache offscreen_target_cache_;
    VkQueryPool ts_pool_ = VK_NULL_HANDLE;
    float ts_period_ns_ = 0.0f;
    bool host_query_reset_ = false;
    PFN_vkResetQueryPool vk_reset_query_pool_ = nullptr;
    std::vector<std::array<const char*, kMaxPasses>> pass_names_;
    std::vector<uint32_t> pass_count_;
    std::vector<uint32_t> compute_pass_count_;
};

}  // namespace cairns::rhi
