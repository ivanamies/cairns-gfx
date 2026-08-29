// rhi/webgpu/swap_resolve_target_plat.hpp
//
// The swap pass's render target: a WGPUTextureView (the offscreen final_target_
// in headless, or the surface's current texture view when windowed) + the
// owning WGPUTexture for readback/present.
#pragma once

// Opaque-handle forward declarations instead of webgpu.h -- the pure-CPU spec
// tests reach this header with no backend include dirs (same pattern as
// resource_manager_plat.hpp).
extern "C" {
typedef struct WGPUTextureViewImpl* WGPUTextureView;
typedef struct WGPUTextureImpl* WGPUTexture;
}

namespace cairns::rhi {

struct SwapResolveTargetPlat {
    WGPUTextureView view = nullptr;
    WGPUTexture texture = nullptr;
};

inline SwapResolveTargetPlat MakeSwapResolveTargetPlatFromView(
    WGPUTextureView view, WGPUTexture texture) {
    SwapResolveTargetPlat p;
    p.view = view;
    p.texture = texture;
    return p;
}

}  // namespace cairns::rhi
