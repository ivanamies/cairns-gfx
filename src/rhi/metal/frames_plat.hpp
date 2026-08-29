// rhi/metal/frames_plat.hpp

#pragma once

#include <Metal/Metal.hpp>

#include "rhi/resource_manager.hpp"  // Handle<>, Texture

namespace cairns::rhi {

struct FramesPlat {
    MTL::Device* device_ = nullptr;             // mirrored from Device
    MTL::CommandQueue* queue_ = nullptr;        // mirrored from Device
    void* frame_semaphore_ = nullptr;           // dispatch_semaphore_t
    MTL::RenderPassDescriptor* render_pass_desc_ = nullptr;
    MTL::DepthStencilState* depth_stencil_ = nullptr;
    Handle<Texture> msaa_handle_ = Handle<Texture>::Null;
    Handle<Texture> depth_handle_ = Handle<Texture>::Null;
};

}  // namespace cairns::rhi
