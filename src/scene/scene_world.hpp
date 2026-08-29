#pragma once

#include "scene/scene_entity.hpp"
#include "util/gltf_loader.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

namespace cairns {

struct SceneWorld {
    std::vector<SceneEntity> entities;
    const Scene* scenes = nullptr;
    size_t scene_count = 0;
    glm::mat4 root_transform = glm::mat4(1.0f);
};

}  // namespace cairns
