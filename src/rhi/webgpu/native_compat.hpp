// rhi/webgpu/native_compat.hpp
//
// The one API gap between wgpu-native (desktop) and the browser's emdawnwebgpu:
// wgpu-native exposes wgpuDevicePoll (from the wgpu.h extension header) to block
// until the GPU drains; the browser has no such call -- work drains via the
// requestAnimationFrame loop + async callbacks. Wrapped here once so the backend
// .cpp files stay free of platform guards.
#pragma once

#include <webgpu/webgpu.h>
#ifndef __EMSCRIPTEN__
#include <webgpu/wgpu.h>
#endif

namespace cairns::rhi::webgpu {

// Block until the device's submitted work completes (desktop); no-op in the
// browser, where the rAF loop + spontaneous callbacks advance the queue.
inline void DrainGpu(WGPUDevice device) {
#ifndef __EMSCRIPTEN__
    wgpuDevicePoll(device, /*wait=*/true, nullptr);
#else
    (void)device;
#endif
}

}  // namespace cairns::rhi::webgpu
