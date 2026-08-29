// rhi/command_recorder.hpp
//
// Fork C: pass-scaffolded, data-driven command recording. BeginFrame returns a
// FrameContext whose CommandRecorder exposes per-pass calls; the backend owns the
// bind/draw loop (last-bound caching). See the merge plan for rationale.

#pragma once

#include <cstdint>

#include "rhi/resource_manager.hpp"
#include "util/draw.hpp"

namespace cairns::rhi {

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
    std::span<const uint32_t> sorted_indices;
    Handle<Shader> pipeline;
    Handle<BindGroup> bindless;
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
    void Dispatch(const ComputeDispatch& d);
    void BeginRenderPass(const RenderPassDesc& desc);
    void DrawMeshes(const MeshDrawList& list);
    void DrawPoints(const PointDraw& draw);
    void EndRenderPass();

private:
    friend class ResourceManager;
    struct Impl;
    Impl* impl_ = nullptr;
};

struct FrameContext {
    CommandRecorder cmd;
    uint32_t frame_index = 0;
    uint32_t swapchain_image_index = 0;
};

}  // namespace cairns::rhi
