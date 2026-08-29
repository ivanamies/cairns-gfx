// rhi/vulkan/command_recorder_plat.hpp

#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include <vulkan/vulkan.h>

#include "rhi/resource_manager.hpp"  // kMaxStepsPerFrame

namespace cairns::rhi {

inline constexpr uint32_t kMaxPasses = 16;
// Per-frame ring of composite descriptor sets. Lets a single pass issue
// multiple DrawFullscreen calls with different texture bindings without
// last-bound-wins aliasing (the 997af20 fix).
inline constexpr uint32_t kCompositeRingSize = 4;

// Persistent (owned by Frames) cache of offscreen VkRenderPass + VkFramebuffer
// objects keyed by attachment formats/load-ops and image views. Swapchain
// passes keep using sc.renderPass; only graph-created offscreen targets land
// here.
struct OffscreenTargetCache {
    struct RpKey {
        VkFormat color = VK_FORMAT_UNDEFINED;
        VkFormat depth = VK_FORMAT_UNDEFINED;
        VkAttachmentLoadOp color_load = VK_ATTACHMENT_LOAD_OP_CLEAR;
        VkAttachmentLoadOp depth_load = VK_ATTACHMENT_LOAD_OP_CLEAR;
        bool has_color = false;
        bool has_depth = false;
    };
    struct RpEntry {
        RpKey key;
        VkRenderPass rp = VK_NULL_HANDLE;
    };
    struct FbEntry {
        VkRenderPass rp = VK_NULL_HANDLE;
        VkImageView v0 = VK_NULL_HANDLE;
        VkImageView v1 = VK_NULL_HANDLE;
        uint32_t w = 0;
        uint32_t h = 0;
        VkFramebuffer fb = VK_NULL_HANDLE;
    };
    VkDevice device = VK_NULL_HANDLE;
    std::vector<RpEntry> rps;
    std::vector<FbEntry> fbs;

    void Deinit();
    // Resize entry. Framebuffers are sized at create-time; on resize their
    // (w, h) no longer match the post-resize attachment views. Render passes
    // are keyed on attachment formats / load-ops only, so they can be kept.
    // CALLER: Engine::ApplyPendingResize.
    void FlushFramebuffers();
};

struct CommandRecorderPlat {
    uint32_t frame_ = 0;
    uint32_t image_index_ = 0;
    VkCommandBuffer gfx_ = VK_NULL_HANDLE;
    VkCommandBuffer comp_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkDescriptorSet globals_set_ = VK_NULL_HANDLE;
    VkDescriptorSet drawtmp_set_ = VK_NULL_HANDLE;
    std::array<VkDescriptorSet, kMaxStepsPerFrame> compute_sets_{};
    VkDescriptorSet point_set_ = VK_NULL_HANDLE;
    // Composite descriptor ring for DrawFullscreen (multiple per-pass draws
    // with distinct textures). Advanced by composite_next_idx_ on each
    // DrawFullscreen.
    std::array<VkDescriptorSet, kCompositeRingSize> composite_sets_{};
    uint32_t composite_next_idx_ = 0;
    OffscreenTargetCache* offscreen_ = nullptr;  // owned by Frames
    // Per-pass timing (populated by Frames::Begin; written by PassTimerBegin).
    VkQueryPool ts_pool_ = VK_NULL_HANDLE;
    std::array<const char*, kMaxPasses>* pass_names_ = nullptr;
    uint32_t* pass_count_ = nullptr;
    VkCommandBuffer pass_cb_ = VK_NULL_HANDLE;
    uint32_t pending_pass_idx_ = UINT32_MAX;
};

}  // namespace cairns::rhi
