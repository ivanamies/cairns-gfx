// rhi/metal/command_recorder_plat.hpp

#pragma once

namespace MTL {
class CommandBuffer;
class RenderCommandEncoder;
class RenderPassDescriptor;
class DepthStencilState;
class CommandQueue;
}

namespace cairns::rhi {

struct CommandRecorderPlat {
    MTL::CommandBuffer* cmd_ = nullptr;
    MTL::RenderCommandEncoder* enc_ = nullptr;
    MTL::RenderPassDescriptor* render_pass_desc_ = nullptr;
    MTL::DepthStencilState* depth_stencil_ = nullptr;
    // Per-pass timing (populated by Frames::Begin; lazy-acquired cmd buffer).
    MTL::CommandQueue* queue_ = nullptr;
};

}  // namespace cairns::rhi
