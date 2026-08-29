// rhi/metal/command_recorder_impl.hpp
//
// Internal: defines CommandRecorder::Impl for the Metal backend. Shared between
// metal/command_recorder.cpp (the method bodies) and metal/resource_manager.cpp
// (which constructs/destroys the Impl in BeginFrame/EndFrame). Not a public
// header — it leaks MTL:: types and must only be included by Metal backend .cpp.

#pragma once

#include "util/define.hpp"

#if CAIRNS_METAL

#include <Metal/Metal.hpp>

#include "rhi/command_recorder.hpp"

namespace cairns::rhi {

struct CommandRecorder::Impl {
    ResourceManager* rm = nullptr;
    SwapChain* sc = nullptr;
    MTL::CommandBuffer* cmd = nullptr;
    MTL::RenderCommandEncoder* enc = nullptr;
    MTL::RenderPassDescriptor* render_pass_desc = nullptr;
    MTL::DepthStencilState* depth_stencil = nullptr;
};

}  // namespace cairns::rhi

#endif  // CAIRNS_METAL
