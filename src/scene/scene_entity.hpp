#pragma once

#include "render/render_proxy.hpp"

#include <glm/glm.hpp>

#include <cstdint>

namespace cairns {

struct SceneEntity {
    glm::mat4 transform = glm::mat4(1.0f);
    uint32_t scene_index = 0;
    uint32_t layer_mask = 0xFFFFFFFFu;
    uint32_t flags = kProxyVisible;
};

}  // namespace cairns
