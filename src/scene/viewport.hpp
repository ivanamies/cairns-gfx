// scene/viewport.hpp
//
// A Viewport is the rendered pane: a Camera + offscreen color target
// looking at a World. Editor-engine vocabulary (Document-View):
// World = Document (data + edit state); Viewport = the pane onto it;
// Camera = the math. Editors keep N of these (one per dock pane). The
// camera is per-viewport, NOT per-world -- multiple panes can show the
// same world from different angles.
//
// Target lifetime: PERSISTENT imported texture, NOT a per-frame
// transient. Allocated on viewport open, sized to the pane; re-allocated
// on resize. The render graph imports it each frame.

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

struct Viewport {
    WorldId world;
    Camera camera;
    rhi::Handle<rhi::Texture> target;
    uint32_t target_w = 0;
    uint32_t target_h = 0;
    bool camera_dirty = true;
};

}  // namespace cairns
