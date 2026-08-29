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
    // Same problem for graphics->graphics: untracked render targets mean the
    // encoder boundary does NOT sync a prior render pass's writes against the
    // next pass that reads them (e.g. forward writes color_off, swap samples
    // it). EndRenderPass updates this, BeginRenderPass waits it -- the metal
    // mirror of the vk backend's per-pass transition() barrier. FLAKY_TESTS #2.
    MTL::Fence* gfx_fence_ = nullptr;
};

}  // namespace cairns::rhi
