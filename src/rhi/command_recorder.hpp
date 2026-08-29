// rhi/command_recorder.hpp
//
// Fork C: pass-scaffolded, data-driven command recording. BeginFrame returns a
// FrameContext whose CommandRecorder exposes per-pass calls; the backend owns the
// bind/draw loop (last-bound caching). See the merge plan for rationale.

#pragma once

#include <cstdint>
#include <utility>
#include <vector>

#include "rhi/resource_manager.hpp"
#include "util/draw.hpp"
#include "util/draw_key.hpp"
#if CAIRNS_VULKAN
#include <vulkan/vulkan.h>
#elif CAIRNS_METAL
#include <Metal/Metal.hpp>
#endif

struct ImDrawData;

namespace cairns::rhi {

class Resources;
class Allocator;
struct SwapChain;

// Per-frame ring of fullscreen-sampling descriptor sets: each DrawFullscreen in a
// frame needs its own set, since a set is referenced by recorded draws but
// updated in place (reusing one set => all draws sample the last write).
inline constexpr uint32_t kCompositeRing = 4;

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
    void BeginRenderPass(Resources& res, SwapChain& sc, const RenderPassDesc& desc);
    void DrawMeshes(Resources& res, Allocator& alloc, const MeshDrawList& list);
    void DrawPoints(Resources& res, Allocator& alloc, const PointDraw& draw);
    // Fullscreen triangle that binds `tex_count` sampled textures (set 0) + one
    // shared sampler and draws 3 verts. No vertex buffers.
    void DrawFullscreen(Resources& res, Handle<Shader> pipeline,
                        const Handle<Texture>* textures, uint32_t tex_count,
                        Handle<Sampler> sampler);
    // Render ImGui draw data through the RHI (own pipeline + per-frame bump
    // upload of vtx/idx + per-cmd scissor). Drawn inside the swapchain pass.
    void DrawImGui(Resources& res, Allocator& alloc, Handle<Shader> pipeline,
                   Handle<Texture> font, Handle<Sampler> sampler,
                   const ImDrawData* draw_data);
    void SetViewport(float x, float y, float w, float h);
    void SetScissor(int32_t x, int32_t y, uint32_t w, uint32_t h);
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
    VkDescriptorSet compute_set_ = VK_NULL_HANDLE;
    VkDescriptorSet point_set_ = VK_NULL_HANDLE;
    VkDescriptorSet composite_set_ring_[kCompositeRing] = {};
    uint32_t composite_set_cursor_ = 0;
    VkDescriptorSet imgui_set_ = VK_NULL_HANDLE;
    OffscreenTargetCache* offscreen_ = nullptr;  // owned by Frames
#elif CAIRNS_METAL
    MTL::CommandBuffer* cmd_ = nullptr;
    MTL::RenderCommandEncoder* enc_ = nullptr;
    MTL::RenderPassDescriptor* render_pass_desc_ = nullptr;
    MTL::DepthStencilState* depth_stencil_ = nullptr;
#endif
};

struct FrameContext {
    CommandRecorder cmd;
    uint32_t frame_index = 0;
    uint32_t swapchain_image_index = 0;
};

}  // namespace cairns::rhi
