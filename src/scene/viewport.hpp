// scene/viewport.hpp
//
// A Viewport is the rendered pane: a Camera + offscreen color target
// bound to a Scene. Editor-engine vocabulary (Document-View): Scene =
// Document (data + edit state); Viewport = the pane onto it; Camera =
// the math. Editors keep N of these (one per dock pane / [N-node] graph
// node). The camera is per-viewport, NOT per-scene -- multiple panes
// can show the same scene from different angles.
//
// Target lifetime: PERSISTENT imported texture, NOT a per-frame
// transient. Allocated on viewport open, sized to the pane; re-allocated
// on resize. The render graph imports it each frame. The per-viewport
// target is the multi-scene seam: a composition pane samples it.

#pragma once

#include "core/handle.hpp"
#include "rhi/resource_manager.hpp"
#include "scene/world.hpp"

#include <entt/entt.hpp>
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

// Per-viewport navigation state. yaw rotates around world up (Y); pitch around
// the camera's local right (X). The resolved view matrix on Viewport::camera
// is the durable thing -- this struct is just the input to that resolve.
struct FlyController {
    glm::vec3 position{0.0f, 0.0f, 0.0f};
    float yaw = 0.0f;    // radians
    float pitch = 0.0f;  // radians
};

// Handle-pilled (ViewportId = Handle<Viewport>), owned by Engine's viewport
// pool. Hot = per-draw read in RecordFrame; Cold = camera-entity binding +
// fly controller, both read once per frame.
struct Viewport {
    struct Hot {
        SceneId scene;
        Camera camera;
        rhi::Handle<rhi::Texture> target;
        rhi::Handle<rhi::Texture> depth_target;
        uint32_t target_w = 0;
        uint32_t target_h = 0;
        // Where this viewport tiles on the swap pane, in NDC (0..1).
        // (x, y) = bottom-left corner; (z, w) = size. {0,0,1,1} = full pane.
        // Default for viewport 0 = full pane; other viewports = zero-size so
        // they're inert until the agent calls cairns.viewport.setLayout.
        glm::vec4 layout_rect{0.0f, 0.0f, 0.0f, 0.0f};
        bool camera_dirty = true;
    };
    struct Cold {
        // Camera role #2: when non-null, the viewport's Camera is resolved
        // from this entity's WorldTransform + CameraComponent each frame.
        // entt::null means "use FlyController" (Camera role #1). Resolution
        // happens once per frame in BuildMeshOpaqueDraws.
        entt::entity camera_entity = entt::null;
        FlyController fly;
        // Per-viewport particle DRAW toggle: gates only the DrawPoints
        // call in this viewport's forward pass (the sim/compute gate is
        // separate). Default true so the sim gate alone decides; two
        // viewports on one scene can differ (e.g. left off, right on).
        bool particles_enabled = true;
        // Per-viewport editor-chrome gate. The selection-outline pass
        // draws for this viewport only when set. Default on. [N-node]
        // node K can drop its outline for a capture while node J keeps
        // it.
        bool chrome_enabled = true;
    };
};

using ViewportId = Handle<Viewport>;

}  // namespace cairns
