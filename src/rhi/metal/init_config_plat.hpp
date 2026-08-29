// rhi/metal/init_config_plat.hpp

#pragma once

namespace CA { class MetalLayer; }

namespace cairns::rhi {

struct InitConfigPlat {
    // Pre-resolved Metal layer (shell does SDL_Metal_CreateView +
    // SDL_Metal_GetLayer). Required when surfaceless == false.
    CA::MetalLayer* metal_layer = nullptr;
};

}  // namespace cairns::rhi
