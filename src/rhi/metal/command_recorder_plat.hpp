// rhi/metal/command_recorder_plat.hpp

#pragma once

namespace MTL {
class CommandBuffer;
class RenderCommandEncoder;
class RenderPassDescriptor;
class DepthStencilState;
class CommandQueue;
class Fence;
}

namespace cairns::rhi {

struct CommandRecorderPlat {
    MTL::CommandBuffer* cmd_ = nullptr;
    MTL::RenderCommandEncoder* enc_ = nullptr;
    MTL::RenderPassDescriptor* render_pass_desc_ = nullptr;
    MTL::DepthStencilState* depth_stencil_ = nullptr;
    // Per-pass timing (populated by Frames::Begin; lazy-acquired cmd buffer).
    MTL::CommandQueue* queue_ = nullptr;
    // Lazy-created. anim_eval -> skin compute cross-encoder synchronization.
    // Buffers are HazardTrackingModeUntracked (heap-wide setting), so encoder
    // boundary does NOT auto-sync compute writes. DispatchAnimEval updates,
    // DispatchSkinBatches waits.
    MTL::Fence* compute_fence_ = nullptr;
    // graphics->graphics sync is now per-resource + graph-driven: each render
    // target carries its own MTL::Fence in TextureColdPlat::sync_fence_, signaled
    // by the writing pass (EndRenderPass) and waited by the next hazarding pass
    // (BeginRenderPass) from the graph's computed ResourceBarrier list.
};

}  // namespace cairns::rhi
