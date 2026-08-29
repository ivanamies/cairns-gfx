// engine/anim_skin_system.hpp
//
// GPU skinning + animation state, grouped out of the Engine god class (C2 S2):
// the SkinnedAttachment pool + persistent skin-output RangePool, the skin
// deform + palette-eval kernels, the folded anim-table SSBOs, the delta-upload
// cursor/dirty latches, and the dynamic-offset descriptor sets. Engine's load +
// BuildSkinFrame + uploadAnimTablesGpu systems operate on it.

#pragma once

#include <cstdint>

#include "rhi/resource_manager.hpp"
#include "util/cpu_pool.hpp"        // RangePool
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
};

}  // namespace cairns
