#pragma once

#include "render/render_proxy_arrays.hpp"
#include "rhi/resource_manager.hpp"

#include <vector>

namespace cairns {

struct RenderScene {
    RenderProxyArrays proxies;
    std::vector<rhi::Handle<rhi::Texture>> resident_textures;

    void Clear() {
        proxies.Clear();
        resident_textures.clear();
    }
};

}  // namespace cairns
