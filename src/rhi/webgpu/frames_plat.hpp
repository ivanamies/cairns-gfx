// rhi/webgpu/frames_plat.hpp
#pragma once

#include <atomic>

#include <webgpu/webgpu.h>

#include "rhi/resource_manager.hpp"  // Handle<>, Texture

namespace cairns::rhi {

struct FramesPlat {
    WGPUDevice device_ = nullptr;   // mirrored from Device
    WGPUQueue queue_ = nullptr;     // mirrored from Device
    Handle<Texture> msaa_handle_ = Handle<Texture>::Null;
    Handle<Texture> depth_handle_ = Handle<Texture>::Null;
    std::atomic<double> last_gpu_end_s_{0.0};
};

}  // namespace cairns::rhi
