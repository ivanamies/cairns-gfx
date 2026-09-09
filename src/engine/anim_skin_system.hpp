// engine/anim_skin_system.hpp
//
// GPU skinning + animation state: the SkinnedAttachment pool + persistent
// skin-output RangePool, the skin deform + palette-eval kernels, the folded
// anim-table SSBOs, the delta-upload cursor/dirty latches, and the
// dynamic-offset descriptor sets. Pure state; Engine's load + BuildSkinFrame
// + uploadAnimTablesGpu systems operate on it.

#pragma once

#include <cstdint>
#include <vector>

#include "rhi/resource_manager.hpp"
#include "util/chunk_allocator.hpp"  // ChunkStdAllocator
#include "util/cpu_pool.hpp"        // RangePool
#include "util/gpu_anim_types.hpp"  // GpuTRS
#include "render/render_proxy.hpp"  // SkinnedAttachment

namespace cairns {

// Anim-table delta-upload cursors (elements uploaded per folded SSBO).
struct AnimCursors {
    uint32_t headers = 0;
    uint32_t i32 = 0;
    uint32_t vec4 = 0;
    uint32_t word16 = 0;
};

struct AnimSkinSystem {
    // Per-actor skin records + the 256 MB persistent GPU output pool
    // (RangePool in vec4 vertex units over output_pool_buffer).
    cairns::ResourceManager<cairns::SkinnedAttachment> skins;
    rhi::Handle<rhi::Buffer> output_pool_buffer;
    cairns::RangePool output_pool;

    // Skin-deform + GPU palette-eval kernels + the folded anim-table SSBOs
    // (9 per-type buffers -> 3 by element type; scene headers shared).
    rhi::Handle<rhi::Kernel> skin_kernel;
    rhi::Handle<rhi::Kernel> eval_kernel;
    rhi::Handle<rhi::Buffer> scene_headers_buf;
    rhi::Handle<rhi::Buffer> ae_i32_buf;
    rhi::Handle<rhi::Buffer> ae_vec4_buf;
    rhi::Handle<rhi::Buffer> ae_word16_buf;
    rhi::Handle<rhi::Buffer> world_scratch_buf;
    rhi::Handle<rhi::Buffer> palette_out_buf;

    // Aaltonen delta-upload cursor state + dirty/warn latches.
    bool eval_tables_uploaded = false;
    AnimCursors cur;
    uint32_t uploaded_prefab_count = 0;
    bool dyn_dirty = true;
    bool actors_cap_warned = false;

    // Dynamic-offset descriptor sets for the skin group-B + anim-eval binds.
    rhi::Handle<rhi::DynamicBuffers> dyn_skin_group_b;
    rhi::Handle<rhi::DynamicBuffers> dyn_anim_eval;

    // CPU mirror of scene_headers_buf, index-parallel with it. Kept so a pose
    // override can clone an actor's prefab header (its *_off offsets are only
    // knowable at upload time) without re-deriving the whole packing.
    std::vector<cairns::GpuSceneHeader,
                cairns::ChunkStdAllocator<cairns::GpuSceneHeader>> headers_cpu;

    // ---- Per-actor joint-pose overrides -----------------------------------
    // A pose override is a full node-local TRS block for ONE actor, appended
    // to the packed ae_vec4 table, plus a GpuSceneHeader clone of that actor's
    // prefab header whose bind_pose_off points at the block and whose
    // channel_count is 0. anim_eval then seeds stage 1 from the override and
    // stage 2 (clip sampling) has nothing to sample -- the clip is bypassed
    // with no shader edit, no extra binding and no field on GpuActorRecord.
    // Repointing SkinnedAttachment::Hot::gpu_prefab_header_idx at the clone is
    // what switches an actor over; restoring the prefab's index clears it.
    //
    // Acton: parallel arrays keyed by position, one entry per overridden
    // actor, plus one flat TRS blob. pose_skins is the key -- a scripted scene
    // has a handful of posed actors, so the lookup is a linear scan, not a map.
    std::vector<cairns::SkinId,
                cairns::ChunkStdAllocator<cairns::SkinId>> pose_skins;
    // vec4-element offset of this actor's block inside ae_vec4_buf.
    std::vector<uint32_t, cairns::ChunkStdAllocator<uint32_t>> pose_vec4_off;
    // Index of this actor's clone inside scene_headers_buf.
    std::vector<uint32_t, cairns::ChunkStdAllocator<uint32_t>> pose_header_idx;
    // Element offset of this actor's block inside pose_trs (CPU mirror), and
    // its node count -- the block is node_count GpuTRS long.
    std::vector<uint32_t, cairns::ChunkStdAllocator<uint32_t>> pose_trs_off;
    std::vector<uint32_t, cairns::ChunkStdAllocator<uint32_t>> pose_node_count;
    // 1 while the actor is actually pointed at its clone. A cleared actor
    // keeps its slot (the packed tables are append-only) but follows its clip
    // again, and a rebuild must not re-hijack it.
    std::vector<uint8_t, cairns::ChunkStdAllocator<uint8_t>> pose_active;
    // CPU mirror of every override block, so a later full table rebuild can
    // re-append them and a partial pose update can patch one node in place.
    std::vector<cairns::GpuTRS,
                cairns::ChunkStdAllocator<cairns::GpuTRS>> pose_trs;
};

}  // namespace cairns
