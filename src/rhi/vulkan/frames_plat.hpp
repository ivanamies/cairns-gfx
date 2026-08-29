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
    VkDescriptorSetLayout point_layout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout composite_set_layout_ = VK_NULL_HANDLE;
    // #222 Phase D.3 cleanup: skin Group B + anim_eval layouts kept here
    // because initSkinKernel + initAnimEvalKernel build their pipelines
    // BEFORE uploadAnimTablesGpu creates the backing buffers for
    // dyn_skin_group_b_ / dyn_anim_eval_. The DynamicBuffers descriptor
    // sets allocated post-scene-load are layout-compatible. compute_layout_
    // (particle) IS retired: initParticles creates dyn_particle_parity_
    // before the kernel, so the kernel sources its layout from there.
    VkDescriptorSetLayout skin_group_b_layout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout anim_eval_layout_ = VK_NULL_HANDLE;
    // Group A is per-mesh (SSBO positions @0, SSBO skin-attrs @1) -- one
    // set per skinned mesh, allocated at load via Resources::CreateBindGroup
    // and stored on Mesh::Hot.
    VkDescriptorSetLayout skin_group_a_layout_ = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> globals_sets_;
    std::vector<VkDescriptorSet> drawtmp_sets_;
    std::vector<VkDescriptorSet> point_sets_;
    // Per-frame ring of composite descriptor sets for DrawFullscreen. Lets one
    // pass issue multiple fullscreen draws with distinct textures (the 997af20
    // last-bound-wins fix).
    std::vector<std::array<VkDescriptorSet, kCompositeRingSize>> composite_sets_;
};

}  // namespace cairns::rhi
