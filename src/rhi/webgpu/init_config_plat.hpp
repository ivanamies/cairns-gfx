// rhi/webgpu/init_config_plat.hpp
//
// Native headless leaves all of this null: Device::Init requests its own
// instance/adapter/device. The browser/web build CANNOT block on the async
// adapter/device request, so the web entry acquires them up front (callback
// chain) + the canvas surface, and injects them here; Device::Init then adopts
// the provided handles instead of requesting.
#pragma once

#include <webgpu/webgpu.h>

namespace cairns::rhi {

struct InitConfigPlat {
    WGPUInstance instance = nullptr;
    WGPUAdapter adapter = nullptr;
    WGPUDevice device = nullptr;
    WGPUQueue queue = nullptr;
    WGPUSurface surface = nullptr;
    WGPUTextureFormat surface_format = WGPUTextureFormat_BGRA8Unorm;
};

}  // namespace cairns::rhi
