// rhi/webgpu/swap_chain_plat.hpp
//
// Headless webgpu renders offscreen and never constructs a SwapChain; the
// windowed/web build sets `surface` + dimensions and AcquireForFrame pulls the
// canvas's current texture. The common SwapChain forwards Width/Height/Deinit
// and calls AcquireForFrame per frame, so those must exist.
#pragma once

#include <cstdint>

#include <webgpu/webgpu.h>

#include "rhi/swap_resolve_target.hpp"

namespace cairns::rhi {

struct SwapChainPlat {
    WGPUSurface surface = nullptr;  // null in headless
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    WGPUTextureFormat format = WGPUTextureFormat_BGRA8Unorm;

    void Deinit() {}
    uint32_t Width() const { return width_; }
    uint32_t Height() const { return height_; }

    // Windowed only (wgpuSurfaceGetCurrentTexture); headless never calls this.
    SwapResolveTarget AcquireForFrame() { return SwapResolveTarget{}; }
};

}  // namespace cairns::rhi
