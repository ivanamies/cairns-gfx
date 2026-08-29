#pragma once

#include "render/render_proxy_arrays.hpp"
#include "scene/scene_world.hpp"

namespace cairns {

inline void Extract(const SceneWorld& world, RenderProxyArrays& out) {
    (void)world;
    out.Clear();
}

}  // namespace cairns
