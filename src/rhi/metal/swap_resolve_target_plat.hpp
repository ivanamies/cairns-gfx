// rhi/metal/swap_resolve_target_plat.hpp

#pragma once

namespace MTL {
class Texture;
}
namespace CA {
class MetalDrawable;
}

namespace cairns::rhi {

struct SwapResolveTargetPlat {
    // Resolve target for the swap pass + the drawable to present, if any.
    // drawable == nullptr means render-to-texture (no present, sync at End).
    MTL::Texture* texture = nullptr;
    CA::MetalDrawable* drawable = nullptr;
};

inline SwapResolveTargetPlat MakeSwapResolveTargetPlatFromTexture(MTL::Texture* tex) {
    SwapResolveTargetPlat p;
    p.texture = tex;
    p.drawable = nullptr;
    return p;
}

}  // namespace cairns::rhi
