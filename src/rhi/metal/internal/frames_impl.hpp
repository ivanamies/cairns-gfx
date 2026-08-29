// rhi/metal/internal/frames_impl.hpp
//
// Internal: Frames::Impl for Metal. Shared between metal/frames.cpp and
// metal/resource_manager.cpp. Not a public header.

#pragma once

#include "util/define.hpp"

#if CAIRNS_METAL

#include <filesystem>

#include <Metal/Metal.hpp>

#include "rhi/frames.hpp"
#include "rhi/resource_manager.hpp"  // Handle<Texture>

namespace cairns::rhi {

struct Frames::Impl {
    MTL::Device* device = nullptr;       // mirrored from Device
    MTL::CommandQueue* queue = nullptr;  // mirrored from Device
    Resources* res = nullptr;            // borrowed

    std::filesystem::path dump_path;
    void* frame_semaphore = nullptr;  // dispatch_semaphore_t
    MTL::RenderPassDescriptor* render_pass_desc = nullptr;
    MTL::DepthStencilState* depth_stencil = nullptr;
    Handle<Texture> msaa_handle = Handle<Texture>::Null;
    Handle<Texture> depth_handle = Handle<Texture>::Null;
};

}  // namespace cairns::rhi

#endif  // CAIRNS_METAL
