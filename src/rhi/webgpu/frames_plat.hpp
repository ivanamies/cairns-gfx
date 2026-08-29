// rhi/webgpu/frames_plat.hpp
#pragma once

#include <atomic>

#include <webgpu/webgpu.h>

#include "rhi/resource_manager.hpp"  // Handle<>, Texture

namespace cairns::rhi {

namespace webgpu { class MemoryAllocator; }

struct FramesPlat {
    WGPUDevice device_ = nullptr;   // mirrored from Device
    WGPUQueue queue_ = nullptr;     // mirrored from Device
    Handle<Texture> msaa_handle_ = Handle<Texture>::Null;
    Handle<Texture> depth_handle_ = Handle<Texture>::Null;
    // Stashed in Begin so EndSubmit can flush the bump ring (no allocator arg
    // on the EndSubmit seam). Same allocator every frame.
    webgpu::MemoryAllocator* bump_alloc_ = nullptr;
    std::atomic<double> last_gpu_end_s_{0.0};
};

}  // namespace cairns::rhi
