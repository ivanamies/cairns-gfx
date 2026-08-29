// scene/view.hpp
//
// A View is a single viewport: which world it shows, the per-view
// camera, and the offscreen color target it composites onto the
// swapchain. Editors keep >= 4 of these (one per dock pane). The
// camera is per-view, NOT per-world -- multiple panes can show the
// same world from different angles.
//
// Target lifetime: PERSISTENT imported texture, NOT a per-frame
// transient. Allocated on view open, sized to the viewport pane;
// re-allocated on resize. The render graph imports it each frame.

#pragma once

#include "core/handle.hpp"
#include "rhi/resource_manager.hpp"
#include "scene/world.hpp"

#include <glm/glm.hpp>

#include <cstdint>

namespace cairns {

struct Camera {
    glm::mat4 view{1.0f};
    glm::mat4 proj{1.0f};
    glm::vec3 position{0.0f};
    float near_z = 0.1f;
    float far_z = 100.0f;
};

struct View {
    WorldId world;
    Camera camera;
    rhi::Handle<rhi::Texture> target;
    uint32_t target_w = 0;
    uint32_t target_h = 0;
    bool camera_dirty = true;
};

}  // namespace cairns
