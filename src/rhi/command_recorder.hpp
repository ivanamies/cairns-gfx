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
#if CAIRNS_METAL
#include "rhi/metal/command_recorder_plat.hpp"
#elif CAIRNS_VULKAN
#include "rhi/vulkan/command_recorder_plat.hpp"
#elif CAIRNS_WEBGPU
#include "rhi/webgpu/command_recorder_plat.hpp"
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

// A graph-computed barrier on one resource (Granite Barrier + transition). The
// render graph fills these from PipelineEvent state and hands them to the
// backend, which executes them (vk: one vkCmdPipelineBarrier; metal: per-resource
// MTLFence wait). Exactly one of texture/buffer is set.
struct ResourceBarrier {
    Handle<Texture> texture;
    Handle<Buffer> buffer;
    uint32_t src_access = kAccessNone;
    uint32_t dst_access = kAccessNone;
    uint32_t src_stage = kPipeNone;
    uint32_t dst_stage = kPipeNone;
    BarrierLayout old_layout = BarrierLayout::kUndefined;
    BarrierLayout new_layout = BarrierLayout::kUndefined;
};

struct ComputeDispatch {
    Handle<Kernel> kernel;
    uint32_t groups_x = 1;
    uint32_t groups_y = 1;
    uint32_t groups_z = 1;
    // Metal threadsPerThreadgroup (Vulkan ignores; encoded in the SPIR-V).
    uint32_t local_x = 1;
    uint32_t local_y = 1;
    uint32_t local_z = 1;
    uint32_t step_index = 0;
    // #222 Phase D.4: recorder binds set 0 from
    // GetHot(dyn_set_0)->plat.vk_sets[frame_] with dyn_offset_0. Caller
    // pre-built the set via DynamicBuffers (binding 0 UBO_DYN dt,
    // bindings 1..N SSBO over persistent backing).
    Handle<DynamicBuffers> dyn_set_0;
    uint32_t dyn_offset_0 = 0;
};

struct MeshDrawList {
    std::span<const cairns::Draw> draws;
    std::span<const std::pair<DrawKey, uint32_t>> sorted_draws;
    Handle<Shader> pipeline;
    uint32_t globals_offset = 0;
    std::span<const Handle<Texture>> resident_textures;
    std::span<const Handle<Buffer>> resident_buffers;
    // #222 Phase D.2: DynamicBuffers handle for set 0 (pass globals).
    // Carries the per-FIF descriptor set + layout that replaces
    // FramesPlat::globals_set_layout_ / globals_sets_. Null = fall back to
    // the legacy frames.plat.globals_set_ binding.
    Handle<DynamicBuffers> dyn_globals;
};

struct PointDraw {
    Handle<Shader> pipeline;
    Handle<Buffer> vertex_buffer;
    uint32_t vertex_offset = 0;
    uint32_t vertex_count = 0;
};

// #221 Skinning Phase 5: render-side batch (resolves arena-relative
// element offsets to kDynamic byte offsets at EncodeDraws time). One per
// (mesh, instance-list) pair. The recorder iterates these, binds the
// mesh's Group A set, sets 3 dynamic offsets on the persistent Group B
// set, and issues one vkCmdDispatch.
struct SkinDispatchBatch {
    Handle<BindGroup> mesh_set;            // Group A (Vulkan path)
    // Metal path: resolved mesh buffer handles + base-vertex byte offsets.
    Handle<Buffer> pos_buffer;
    uint32_t pos_byte_offset = 0;          // = global_base_vertex * sizeof(vec4)
    Handle<Buffer> skin_attr_buffer;
    uint32_t skin_attr_byte_offset = 0;    // = skin_attr_base_vertex * sizeof(SkinVertex 32B)
    uint32_t params_byte_offset = 0;       // dynamic offset for Group B binding 0
    uint32_t palettes_byte_offset = 0;     // dynamic offset for Group B binding 1
    uint32_t instance_meta_byte_offset = 0; // dynamic offset for Group B binding 2
    // #222 Phase D.3: palette_buffer field retired (MISTAKES.md counter:1).
    // It was frame-wide constant masquerading as per-batch state. Recorder
    // now takes palette_buf as a parameter to DispatchSkinBatches.
    uint32_t workgroups = 0;                // workgroups along X axis (per instance)
    uint32_t instance_count = 1;            // dispatched along Y axis
};

class CommandRecorder {
public:
    void Dispatch(Resources& res, Allocator& alloc, const ComputeDispatch& d);
    // #221 Skinning Phase 5: dedicated skin path. Binds pipeline + Group B
    // descriptors ONCE (whole-buffer writes to palettes/InstanceMeta/Params
    // via the kDynamic master buffer + output pool whole), then loops
    // per-batch: vkCmdBindDescriptorSets(set 0 + set 1, 3 dyn offsets) +
    // vkCmdDispatch. Routes into plat.comp_ on Vulkan (free vertex-fetch
    // sync via the existing compute->graphics semaphore @ VERTEX_INPUT).
    // batches.size() == 0 is a no-op; the engine guards on this AND on
    // skin_kernel_.IsNull() to keep the static path bit-for-bit.
    // #222 Phase D.3: palette_buf is the frame-wide palette destination
    // (palette_out_buf_). Was per-batch on SkinDispatchBatch; promoted to
    // a parameter since it doesn't vary across batches.
    void DispatchSkinBatches(Resources& res, Allocator& alloc,
                              Handle<Kernel> kernel,
                              Handle<Buffer> output_pool_buffer,
                              Handle<Buffer> palette_buf,
                              Handle<DynamicBuffers> dyn_set_0,
                              std::span<const SkinDispatchBatch> batches);
    // #221 Phase 5b: dispatch anim_eval (one workgroup per actor; 64 threads
    // per workgroup). Persistent scene-table SSBOs + actor_records dynUBO are
    // bound through the anim_eval descriptor set on vk and directly as
    // buffers on Metal. records_byte_offset selects this frame's slice in
    // the kDynamic ring (vk); records_buffer / records_byte_offset together
    // are read by Metal directly.
    // #222 Phase R.1: 13-buffer + 2-uint param sprawl collapsed to a
    // parameter object. Add fields here, not to the signature.
    struct AnimEvalArgs {
        Handle<Buffer> scene_headers;
        Handle<Buffer> parent_buf;
        Handle<Buffer> topo_buf;
        Handle<Buffer> bind_pose_buf;
        Handle<Buffer> channels_buf;
        Handle<Buffer> samplers_buf;
        Handle<Buffer> times_buf;
        Handle<Buffer> values_buf;
        Handle<Buffer> joint_nodes_buf;
        Handle<Buffer> inverse_binds_buf;
        Handle<Buffer> world_scratch;
        Handle<Buffer> palette_out;
        // #222 Phase D.3: dyn_set_0 carries the per-FIF anim_eval set
        // (DynamicBuffers Hot owns the layout + 1 set per frame-in-flight).
        Handle<DynamicBuffers> dyn_set_0;
        uint32_t records_byte_offset = 0;
        uint32_t actor_count = 0;
    };
    void DispatchAnimEval(Resources& res, Allocator& alloc,
                          Handle<Kernel> kernel, const AnimEvalArgs& args);
    // invalidate = graph-computed barriers to apply BEFORE this pass (Granite
    // invalidate bucket). vk: one vkCmdPipelineBarrier; metal: per-resource
    // MTLFence waits.
    void BeginRenderPass(Resources& res, const SwapResolveTarget& target,
                          const RenderPassDesc& desc,
                          std::span<const ResourceBarrier> invalidate);
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
    void PassTimerBegin(const char* name, bool is_compute = false);
    void PassTimerEnd();
    // flush = textures this pass WROTE; backend records the producer side of the
    // barrier (metal: MTLFence signal; vk: no-op, state tracked in PipelineEvent).
    void EndRenderPass(Resources& res, std::span<const Handle<Texture>> flush);

    // Per-frame recording state, populated by Frames::Begin. Backend state in
    // plat; pending_name_/pending_slot_ are common timer state.
    CommandRecorderPlat plat;
    const char* pending_name_ = nullptr;
    int pending_slot_ = -1;
};

struct FrameContext {
    CommandRecorder cmd;
    uint32_t frame_index = 0;
    uint32_t swapchain_image_index = 0;
    bool skip_frame = false;
};

}  // namespace cairns::rhi
