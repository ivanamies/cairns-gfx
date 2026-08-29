#pragma once

#include <glm/glm.hpp>

namespace cairns::rhi {

struct RenderPassGlobals {
    glm::mat4 view_proj;
    glm::mat4 inv_view_proj; // get world-pos form depth for like raycasting
    glm::vec4 camera_pos; // [x, y, z, exposure];
    glm::vec4 camera_dir; // [x, y, z, near plane];
    glm::vec4 screen_params; // [width, height, 1/width, 1/height];
    // Directional light, APPENDED so older shaders' shorter declared prefix
    // still binds. Zeroed when the scene has no DirectionalLight (lit
    // materials render ambient-black; existence-based, no default light).
    glm::mat4 light_view_proj; // shadow matrix (declared with the light so
                               // the struct changes once; the shadow pass
                               // fills it)
    glm::vec4 light_dir;    // [x, y, z normalized; w = shadow_strength]
    glm::vec4 light_color;  // [r, g, b; w = intensity]
    glm::vec4 ambient;      // [r, g, b; w unused]
};

} // namespace cairns::rhi
