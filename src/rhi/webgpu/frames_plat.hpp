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
    // Windowed/web present-by-copy: the canvas/native surface (null in headless
    // cairns_serve -> Present no-ops). Mirrored from Device once the shell
    // provides them; Present copies the offscreen final_target here each frame.
    WGPUSurface surface_ = nullptr;
    WGPUTextureFormat surface_format_ = WGPUTextureFormat_BGRA8Unorm;
    uint32_t surface_w_ = 0;
    uint32_t surface_h_ = 0;
};

}  // namespace cairns::rhi
