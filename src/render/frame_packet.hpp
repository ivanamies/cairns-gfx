// src/render/frame_packet.hpp
//
// Per-frame producer/consumer handoff between the game thread and the render
// thread. Owns no GPU data: spans point into per-slot storage on Engine, plain
// scalars are by value. See the "Architecture -- Acquire the SLOT before
// writing a byte" section of the threading plan.

#pragma once

#include "core/handle.hpp"
#include "rhi/resources.hpp"
#include "util/draw.hpp"
#include "util/draw_key.hpp"

#include <cstdint>
#include <filesystem>
#include <span>
#include <utility>

#include <glm/glm.hpp>

struct ImDrawData;

namespace cairns {

struct Mesh;

// #221 Skinning Phase 5: per-mesh skin dispatch unit. The game thread
// buckets visible skinned actors by MeshId (flat array via prefix sum,
// no map) and emits one SkinBatchGpu per mesh. Render thread feeds the
// 3 dynamic offsets (palettes, InstanceMeta, Params) + binds the
// mesh's Group A set + dispatches one workgroup grid. Element offsets
// are arena-relative (palettes/InstanceMeta), converted to kDynamic
// byte offsets render-side before binding.
struct SkinBatchGpu {
    rhi::Handle<rhi::BindGroup> mesh_set;  // Group A (positions + skin-attrs slices) -- Vulkan path
    cairns::Handle<Mesh> mesh;               // Metal/render-side path: resolve buffers + base verts
    uint32_t first_palette_mat4 = 0;        // element index into palettes span
    uint32_t first_meta = 0;                 // element index into instance_meta span
    uint32_t instance_count = 0;
    uint32_t vertex_count = 0;               // mesh vertex count
    uint32_t workgroups = 0;                 // ceil(instance_count * vertex_count / 64)
};

struct FramePacket {
    uint32_t frame_idx = 0;
    uint32_t slot = 0;

    glm::mat4 view{1.0f};
    glm::mat4 proj{1.0f};
    glm::vec3 camera_pos{0.0f};
    glm::vec3 camera_dir{0.0f, 0.0f, -1.0f};
    float near_z = 0.1f;
    float far_z = 100.0f;

    std::span<const Draw> draws;
    std::span<const std::pair<DrawKey, uint32_t>> sorted;
    std::span<const rhi::Handle<rhi::Texture>> resident_textures;

    // #221 Skinning Phase 5: pre-skin compute payload. All spans live on
    // the producer slot's BumpArena and are immutable for the render
    // thread. Empty when no skinned actors are visible OR skin_kernel_
    // failed to load (preserves the static path bit-for-bit).
    std::span<const SkinBatchGpu> skin_batches;
    std::span<const glm::mat4> palettes;       // flat array; per-actor slabs
    std::span<const glm::uvec2> instance_meta;  // {palette_off_mat4s, output_off_vec4s}

    uint32_t sim_steps_this_frame = 0;
    float fixed_dt = 1.0f / 60.0f;

    uint32_t particle_parity_in = 0;
    uint32_t particle_parity_out = 0;

    ImDrawData* imgui_snapshot = nullptr;

    bool request_dump = false;
    std::filesystem::path dump_path;
};

}  // namespace cairns
