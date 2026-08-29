// rhi/swap_resolve_target.hpp
//
// Per-frame value describing where the final pass writes. The engine resolves
// it once at the top of each frame (either from a SwapChain's next drawable or
// from an engine-owned offscreen texture) and hands it to Frames + RenderGraph
// + CommandRecorder. Frames and SwapChain are app-mode-agnostic: there is no
// "headless" toggle on either; the absence of a presentable target is just a
// SwapResolveTarget whose present-side is null.

#pragma once

#include "util/define.hpp"

#include <cstdint>

#if CAIRNS_METAL
#include <Metal/Metal.hpp>
#include <QuartzCore/QuartzCore.hpp>
#elif CAIRNS_VULKAN
#include <vulkan/vulkan.h>
#endif

namespace cairns::rhi {

struct SwapChain;

struct SwapResolveTarget {
    uint32_t width = 0;
    uint32_t height = 0;
#if CAIRNS_METAL
    // Resolve target for the swap pass + the drawable to present, if any.
    // drawable == nullptr means render-to-texture (no present, sync at End).
    MTL::Texture* texture = nullptr;
    CA::MetalDrawable* drawable = nullptr;
#elif CAIRNS_VULKAN
    // Today the vk path is window-bound: the resolve target is the
    // SwapChain's framebuffer at swapchain_image_index. swap_chain == nullptr
    // is reserved for the upcoming render-to-texture vk path; Frames asserts.
    SwapChain* swap_chain = nullptr;
#endif
};

#if CAIRNS_METAL
inline SwapResolveTarget MakeSwapResolveTargetFromTexture(MTL::Texture* tex,
                                                          uint32_t w,
                                                          uint32_t h) {
    SwapResolveTarget t;
    t.width = w;
    t.height = h;
    t.texture = tex;
    t.drawable = nullptr;
    return t;
}
#endif

}  // namespace cairns::rhi
