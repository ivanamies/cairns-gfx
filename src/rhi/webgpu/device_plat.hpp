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
};

}  // namespace cairns::rhi
