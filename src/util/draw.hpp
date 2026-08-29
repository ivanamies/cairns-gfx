#pragma once

#include "rhi/resource_manager.hpp"

#include <array>
#include <cstdint>

namespace cairns {

static constexpr uint32_t kRenderPassGlobalBindSlot = 1;
static constexpr uint32_t kMaterialBindSlot = 2;
static constexpr uint32_t kShaderSpecificBindSlot = 3;
static constexpr uint32_t kDrawTmpBindSlot = 4;

// The draw packet around which all rendering revolves. Taken from Sebastian
// Aaltonen's "Modern Mobile Rendering Architecture" presentation, slide 22.
struct Draw {
    rhi::Handle<rhi::Shader> shader;
    // Bind groups — Aaltonen's presentation, slide 21. Maps onto the old WebGPU
    // 4-bindslot split (Vulkan on Android + WebGPU mandate a minimum of four):
    //   slot 1: render pass global bindings (sun light, camera matrices, shadow maps)
    //   slot 2: material bindings (samplers, textures)
    //   slot 3: shader-specific bindings (e.g. LUTs, particle/skinning SSBOs)
    // The three user-land slots live here; slot 4 is the dynamic_buffers field below.
    std::array<rhi::Handle<rhi::BindGroup>, 3> bind_groups = {};
    // slot 4: dynamic-offset bound buffers — bump-allocated temporaries like UBOs
    // (and r/w SSBOs).
    rhi::Handle<rhi::DynamicBuffers> dynamic_buffers;
    rhi::Handle<rhi::Buffer> index_buffer;
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
