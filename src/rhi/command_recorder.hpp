// rhi/command_recorder.hpp
//
// Fork C: pass-scaffolded, data-driven command recording. BeginFrame returns a
// FrameContext whose CommandRecorder exposes per-pass calls; the backend owns the
// bind/draw loop (last-bound caching). See the merge plan for rationale.

#pragma once

#include <cstdint>
#include <utility>

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

class CommandRecorder {
public:
    void Dispatch(Resources& res, Allocator& alloc, const ComputeDispatch& d);
    void BeginRenderPass(SwapChain& sc, const RenderPassDesc& desc);
    void DrawMeshes(Resources& res, Allocator& alloc, const MeshDrawList& list);
    void DrawPoints(Resources& res, Allocator& alloc, const PointDraw& draw);
    void DrawImGui(Resources& res, Allocator& alloc, Handle<Shader> pipeline,
                   Handle<Texture> font, Handle<Sampler> sampler,
                   const ImDrawData* draw_data);
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
