// rhi/vulkan/command_recorder_plat.hpp

#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include <vulkan/vulkan.h>

#include "rhi/resource_manager.hpp"  // kMaxStepsPerFrame
#include "util/alloc_tags.hpp"
#include "util/print_allocator.hpp"

namespace cairns::rhi {

inline constexpr uint32_t kMaxPasses = 16;
inline constexpr uint32_t kMaxComputePasses = 4;
// Per-frame ring of composite descriptor sets. Lets a single pass issue
// multiple DrawFullscreen calls with different texture bindings without
// last-bound-wins aliasing (the 997af20 fix).
inline constexpr uint32_t kCompositeRingSize = 4;
// Persistent (owned by Frames) cache of offscreen VkRenderPass + VkFramebuffer
// objects keyed by attachment formats/load-ops and image views. Swapchain
// passes keep using sc.renderPass; only graph-created offscreen targets land
// here.
struct OffscreenTargetCache {
    // #206 multi-color: colors[0..color_count) describe each color
    // attachment (kMaxColors hard cap). color_load applies to attachment
    // 0; secondaries share the same load op for now (no use case for
    // mixed yet). When color_count == 0 the renderpass has no color
    // attachment (depth-only). Back-compat: color_count == 1 matches the
    // pre-MRT shape exactly.
    static constexpr uint32_t kMaxColors = 4;
    struct RpKey {
        VkFormat colors[kMaxColors] = {VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED,
                                        VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED};
        VkFormat depth = VK_FORMAT_UNDEFINED;
        VkAttachmentLoadOp color_load = VK_ATTACHMENT_LOAD_OP_CLEAR;
        VkAttachmentLoadOp depth_load = VK_ATTACHMENT_LOAD_OP_CLEAR;
        // #222 Phase A.2: store-ops part of the renderpass-compat key.
        // CRITICAL: without these, two passes that differ only in storeOp
        // would alias the same VkRenderPass and lose the DONT_CARE elision.
        VkAttachmentStoreOp color_store[kMaxColors] = {
            VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_STORE_OP_STORE,
            VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_STORE_OP_STORE};
        VkAttachmentStoreOp depth_store = VK_ATTACHMENT_STORE_OP_STORE;
        uint32_t color_count = 0;
        bool has_depth = false;
    };
    struct RpEntry {
        RpKey key;
        VkRenderPass rp = VK_NULL_HANDLE;
    };
    struct FbEntry {
        VkRenderPass rp = VK_NULL_HANDLE;
        VkImageView views[kMaxColors + 1] = {VK_NULL_HANDLE};
        uint32_t view_count = 0;
        uint32_t w = 0;
        uint32_t h = 0;
        VkFramebuffer fb = VK_NULL_HANDLE;
    };
    VkDevice device = VK_NULL_HANDLE;
    std::vector<RpEntry,
                cairns::print_allocator<RpEntry,
                                        cairns::tags::VkCmdRecorderRps>>
        rps;
    std::vector<FbEntry,
                cairns::print_allocator<FbEntry,
                                        cairns::tags::VkCmdRecorderFbs>>
        fbs;

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
    // #222 Phase D.3/D.4 cleanup: compute_sets_ + skin_group_b_set_ +
    // anim_eval_set_ retired. Particle, skin Group B, anim_eval all read
    // set 0 from a DynamicBuffers handle passed at dispatch time.
    // #222 Phase E.2: point_set_ retired (particle PSO has zero
    // descriptor sets in its pipeline layout).
    // Composite descriptor ring for DrawFullscreen (multiple per-pass draws
    // with distinct textures). Advanced by composite_next_idx_ on each
    // DrawFullscreen.
    std::array<VkDescriptorSet, kCompositeRingSize> composite_sets_{};
    uint32_t composite_next_idx_ = 0;
    OffscreenTargetCache* offscreen_ = nullptr;  // owned by Frames
    // #222 Phase F.1: Per-pass timing (populated by Frames::Begin; written
    // by PassTimerBegin). Grouped into a profiler_ sub-struct so the GPU
    // profiler state is named as a unit on both Frames + CommandRecorder.
    struct Profiler {
        VkQueryPool ts_pool_ = VK_NULL_HANDLE;
        std::array<const char*, kMaxPasses>* pass_names_ = nullptr;
        uint32_t* pass_count_ = nullptr;
        uint32_t* compute_pass_count_ = nullptr;
    } profiler_;
    VkCommandBuffer pass_cb_ = VK_NULL_HANDLE;
    uint32_t pending_pass_idx_ = UINT32_MAX;
};

}  // namespace cairns::rhi
