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
#include "rhi/metal/swap_resolve_target_plat.hpp"
#elif CAIRNS_VULKAN
#include "rhi/vulkan/swap_resolve_target_plat.hpp"
#endif

namespace cairns::rhi {

struct SwapResolveTarget {
    uint32_t width = 0;
    uint32_t height = 0;
    SwapResolveTargetPlat plat;
};

#if CAIRNS_METAL
inline SwapResolveTarget MakeSwapResolveTargetFromTexture(MTL::Texture* tex,
                                                          uint32_t w,
                                                          uint32_t h) {
    SwapResolveTarget t;
    t.width = w;
    t.height = h;
    t.plat = MakeSwapResolveTargetPlatFromTexture(tex);
    return t;
}
#endif

}  // namespace cairns::rhi
