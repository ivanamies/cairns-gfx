#pragma once

#include "render/render_proxy.hpp"

#include <glm/glm.hpp>

#include <cstdint>

namespace cairns {

// Per allocator D: persistent scene object with stable identity, owned by a
// ResourceManager<SceneEntity> on SceneWorld. Per-draw fields live in Hot;
// editor metadata (names, undo refs) goes in Cold.
struct SceneEntity {
    struct Hot {
        glm::mat4 transform = glm::mat4(1.0f);
        uint32_t scene_index = 0;
        uint32_t layer_mask = 0xFFFFFFFFu;
        uint32_t flags = kProxyVisible;
    };
    struct Cold {
        const char* debug_name = nullptr;
    };
};

}  // namespace cairns
