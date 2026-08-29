#pragma once

#include <glm/glm.hpp>

namespace cairns::rhi {

struct RenderPassGlobals {
    glm::mat4 view_proj;
    glm::mat4 inv_view_proj; // get world-pos form depth for like raycasting
    glm::vec4 camera_pos; // [x, y, z, exposure];
    glm::vec4 camera_dir; // [x, y, z, near plane];
    glm::vec4 screen_params; // [width, height, 1/width, 1/height];
};

} // namespace cairns::rhi
