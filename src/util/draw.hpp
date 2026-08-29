#pragma once

#include "rhi/resource_impl.hpp"

#include <cstdint>

namespace cairns {

static constexpr uint32_t kRenderPassGlobalBindSlot = 1;
static constexpr uint32_t kMaterialBindSlot = 2;
static constexpr uint32_t kShaderSpecificBindSlot = 3;
static constexpr uint32_t kDrawTmpBindSlot = 4;

// "Hypehype Modern Mobile Rendering Architecture" slide 21-22, page 34-36
// the draw packet around which all rendering revolves
struct Draw {
    rhi::Handle<rhi::Shader> shader = rhi::Handle<rhi::Shader>::Null;
    // "Our draw call API exposes three bind group slots to the user land. Vulkan on Android and WebGPU mandate minimum of four bind group slots."
    // slot 1: "The first group has render pass global bindings (sun light, camera matrices, shadow maps, etc)"
    // slot 2: "the second slot has material bindings" like samplers and textures
    // slot 3: "the third slot has shader specific bindings" idk. like LUTs and ssbos for particles and skinning.
    std::array<rhi::Handle<rhi::BindGroup>,3> bind_groups = {};
    // slot 4: "We use the last slot in Vulkan and WebGPU for dynamic offset bound buffers. This is important for bump allocated temporary data, such as uniform buffers." I would put r/w SSBOs here too.
    rhi::Handle<rhi::DynamicBuffers> dynamic_buffers;
    rhi::Handle<rhi::Buffer> index_buffer = rhi::Handle<rhi::Buffer>::Null;
    // slot 1: position
    static constexpr uint32_t kVertexBufferPosSlot = 0;
    // slot 2: ??
    std::array<rhi::Handle<rhi::Buffer>,3> vertex_buffers = {};
    uint32_t index_offset = 0;
    uint32_t vertex_offset = 0;
    uint32_t instance_offset = 0;
    uint32_t instance_count = 1;
    // todo @iamies figure out what this does
    std::array<uint32_t,2> dynamic_buffer_offsets = {};
    uint32_t triangle_count = 0;
};

} // namespace cairns
