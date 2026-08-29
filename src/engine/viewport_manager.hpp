// engine/viewport_manager.hpp
//
// Viewport ownership state, grouped out of the Engine god class (C2 S5): the
// Viewport pool, the per-slot id/name tables, the active viewport index/count,
// and the vpN wire-name counter. Engine's viewport + camera + click-focus
// systems operate on it.
//
// [N-node] A viewport is a "document/canvas": it owns its scene binding, camera
// pose, layout rect, and (on Engine) its per-viewport render targets. This
// grouping keeps that state cohesive; kNumViewports stays small today but the
// arrays are the seam for N.

#pragma once

#include <array>
#include <cstdint>

#include "scene/viewport.hpp"  // Viewport, ViewportId (+ ResourceManager)

namespace cairns {

// Max concurrent viewports. Shared home (was Engine::kNumViewports) so the
// per-viewport id_target render targets on Engine and this manager agree.
inline constexpr int kNumViewports = 4;

// vpN wire-name entry: an engine-assigned monotonic counter mapped to a
// ViewportId. Names are never reused for the engine's process lifetime; the
// RPC layer (scene_ops.cpp) is the only consumer, everywhere internal uses
// ViewportId.
struct ViewportName {
    uint32_t counter = 0;
    cairns::ViewportId id;
};

struct ViewportManager {
    cairns::ResourceManager<cairns::Viewport> pool;
    std::array<cairns::ViewportId, kNumViewports> ids{};
    cairns::ViewportId active;
    int active_index = 0;
    int active_count = 1;  // #194 runtime-live count, default 1
    bool cam_pose_override = false;

    std::array<ViewportName, kNumViewports> names{};
    uint8_t names_count = 0;
    uint32_t next_name = 0;
};

}  // namespace cairns
