#pragma once

#include "render/render_proxy_arrays.hpp"
#include "rhi/resource_manager.hpp"
#include "util/alloc_tags.hpp"
#include "util/print_allocator.hpp"

#include <vector>

namespace cairns {

struct RenderScene {
    RenderProxyArrays proxies;
    std::vector<rhi::Handle<rhi::Texture>,
                cairns::print_allocator<rhi::Handle<rhi::Texture>,
                                        cairns::tags::RenderSceneResidentTextures>>
        resident_textures;

    void Clear() {
        proxies.Clear();
        resident_textures.clear();
    }
};

}  // namespace cairns
