// rhi/webgpu/device_plat.hpp
//
// WebGPU DevicePlat: the instance/adapter/device/queue, plus the surface (only
// set in a windowed/web build; headless leaves it null and renders offscreen).
#pragma once

#include <webgpu/webgpu.h>

namespace cairns::rhi {

struct DevicePlat {
    WGPUInstance instance = nullptr;
    WGPUAdapter adapter = nullptr;
    WGPUDevice device = nullptr;
    WGPUQueue queue = nullptr;
    WGPUSurface surface = nullptr;  // null in headless
    WGPUTextureFormat surface_format = WGPUTextureFormat_BGRA8Unorm;
    // False when the handles were injected by the web entry (it owns + frees
    // them); true when this Device requested them and releases in Deinit.
    bool owns_handles = true;
};

}  // namespace cairns::rhi
