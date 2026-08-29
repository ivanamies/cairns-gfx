// rhi/command_recorder.hpp
//
// Fork C: pass-scaffolded, data-driven command recording. BeginFrame returns a
// FrameContext whose CommandRecorder exposes per-pass calls; the backend owns the
// bind/draw loop (last-bound caching). See the merge plan for rationale.

#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include "rhi/resource_manager.hpp"
#include "rhi/swap_resolve_target.hpp"
#include "util/draw.hpp"
#include "util/draw_key.hpp"
#include "util/frame_clock.hpp"
#if CAIRNS_VULKAN
#include <vulkan/vulkan.h>
#elif CAIRNS_METAL
#include <Metal/Metal.hpp>
#endif

struct ImDrawData;

namespace cairns::rhi {

inline constexpr uint32_t kMaxPasses = 16;
// Per-frame ring of composite descriptor sets. Lets a single pass issue
// multiple DrawFullscreen calls with different texture bindings without
// last-bound-wins aliasing (the 997af20 fix).
inline constexpr uint32_t kCompositeRingSize = 4;


class Resources;
class Allocator;
struct SwapChain;

enum class LoadOp : uint8_t { kClear, kLoad, kDontCare };
enum class StoreOp : uint8_t { kStore, kDontCare };

struct ColorAttachment {
    Handle<Texture> target;
    Handle<Texture> msaa;
    float clear[4] = {0, 0, 0, 1};
    LoadOp load = LoadOp::kClear;
    StoreOp store = StoreOp::kStore;
};

struct DepthAttachment {
    Handle<Texture> depth;
    float clear_depth = 1.0f;
    LoadOp load = LoadOp::kClear;
    StoreOp store = StoreOp::kStore;
};

struct RenderPassDesc {
    std::span<const ColorAttachment> color;
    DepthAttachment depth;
    uint32_t width = 0;
    uint32_t height = 0;
    std::span<const Handle<Texture>> input_textures;
};

struct BoundBuffer {
    uint32_t slot = 0;
    Handle<Buffer> buffer;
    uint32_t offset = 0;
};

struct ComputeDispatch {
    Handle<Kernel> kernel;
    std::span<const BoundBuffer> buffers;
    uint32_t groups_x = 1;
    uint32_t groups_y = 1;
    uint32_t groups_z = 1;
    // Metal threadsPerThreadgroup (Vulkan ignores; encoded in the SPIR-V).
    uint32_t local_x = 1;
    uint32_t local_y = 1;
    uint32_t local_z = 1;
    uint32_t step_index = 0;
};

struct MeshDrawList {
    std::span<const cairns::Draw> draws;
    std::span<const std::pair<DrawKey, uint32_t>> sorted_draws;
    Handle<Shader> pipeline;
    uint32_t globals_offset = 0;
    std::span<const Handle<Texture>> resident_textures;
    std::span<const Handle<Buffer>> resident_buffers;
};

struct PointDraw {
    Handle<Shader> pipeline;
    Handle<Buffer> vertex_buffer;
    uint32_t vertex_offset = 0;
    uint32_t vertex_count = 0;
};

#if CAIRNS_VULKAN
// Persistent (owned by Frames) cache of offscreen VkRenderPass + VkFramebuffer
// objects keyed by attachment formats/load-ops and image views. Swapchain passes
// keep using sc.renderPass; only graph-created offscreen targets land here.
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
};
#endif

class CommandRecorder {
public:
    void Dispatch(Resources& res, Allocator& alloc, const ComputeDispatch& d);
    void BeginRenderPass(Resources& res, const SwapResolveTarget& target,
                          const RenderPassDesc& desc);
    void DrawMeshes(Resources& res, Allocator& alloc, const MeshDrawList& list);
    void DrawPoints(Resources& res, Allocator& alloc, const PointDraw& draw);
    void DrawFullscreen(Resources& res, Handle<Shader> pipeline,
                        std::span<const Handle<Texture>> textures,
                        Handle<Sampler> sampler);
    void SetViewport(float x, float y, float w, float h);
    void SetScissor(int32_t x, int32_t y, uint32_t w, uint32_t h);
    void DrawImGui(Resources& res, Allocator& alloc, Handle<Shader> pipeline,
                   Handle<Texture> font, Handle<Sampler> sampler,
                   const ImDrawData* draw_data);
    void PassTimerBegin(const char* name);
    void PassTimerEnd();
    void EndRenderPass();

    // Per-frame recording state, populated by Frames::Begin.
#if CAIRNS_VULKAN
    uint32_t frame_ = 0;
    uint32_t image_index_ = 0;
    VkCommandBuffer gfx_ = VK_NULL_HANDLE;
    VkCommandBuffer comp_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkDescriptorSet globals_set_ = VK_NULL_HANDLE;
    VkDescriptorSet drawtmp_set_ = VK_NULL_HANDLE;
    std::array<VkDescriptorSet, kMaxStepsPerFrame> compute_sets_{};
    VkDescriptorSet point_set_ = VK_NULL_HANDLE;
    // Composite descriptor ring for DrawFullscreen (multiple per-pass draws with
    // distinct textures). Advanced by composite_next_idx_ on each DrawFullscreen.
    std::array<VkDescriptorSet, kCompositeRingSize> composite_sets_{};
    uint32_t composite_next_idx_ = 0;
    OffscreenTargetCache* offscreen_ = nullptr;  // owned by Frames
    // Per-pass timing (populated by Frames::Begin; written by PassTimerBegin).
    VkQueryPool ts_pool_ = VK_NULL_HANDLE;
    std::array<const char*, kMaxPasses>* pass_names_ = nullptr;
    uint32_t* pass_count_ = nullptr;
    VkCommandBuffer pass_cb_ = VK_NULL_HANDLE;
    uint32_t pending_pass_idx_ = UINT32_MAX;
    const char* pending_name_ = nullptr;
    int pending_slot_ = -1;
#elif CAIRNS_METAL
    MTL::CommandBuffer* cmd_ = nullptr;
    MTL::RenderCommandEncoder* enc_ = nullptr;
    MTL::RenderPassDescriptor* render_pass_desc_ = nullptr;
    MTL::DepthStencilState* depth_stencil_ = nullptr;
    // Per-pass timing (populated by Frames::Begin; lazy-acquired cmd buffer).
    MTL::CommandQueue* queue_ = nullptr;
    const char* pending_name_ = nullptr;
    int pending_slot_ = -1;
#endif
};

struct FrameContext {
    CommandRecorder cmd;
    uint32_t frame_index = 0;
    uint32_t swapchain_image_index = 0;
};

}  // namespace cairns::rhi
