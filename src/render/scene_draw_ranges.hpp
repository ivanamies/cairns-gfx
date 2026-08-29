#pragma once

#include <cstdint>
#include <span>

namespace cairns {

// Per-viewport scene draw fan-out (pure -> spec-tested).
//
// When two viewports bind two different scenes, every scene's meshes are
// extracted into ONE proxy union, contiguously: scene k owns proxy meshes
// [mesh_lo, mesh_hi). `proxy_first_draw[m]` is the prefix sum of per-mesh
// primitive counts (the first draw index of mesh m). CarveSceneDrawRanges
// turns each scene's [mesh) range into its [draw) range -- the slice of the
// shared sorted draw list that scene owns. A viewport then renders ONLY its
// bound scene's [draw_lo, draw_hi) sub-span, so one scene/one viewport gets
// every draw and two scenes/two viewports get disjoint halves.
struct SceneDrawSpan {
    uint32_t mesh_lo = 0;
    uint32_t mesh_hi = 0;
    uint32_t draw_lo = 0;
    uint32_t draw_hi = 0;
};

inline void CarveSceneDrawRanges(std::span<const uint32_t> proxy_first_draw,
                                 uint32_t total_draws,
                                 std::span<SceneDrawSpan> scenes) {
    const uint32_t n = static_cast<uint32_t>(proxy_first_draw.size());
    for (SceneDrawSpan& s : scenes) {
        // mesh_hi == n (one past the last mesh) maps to total_draws; the prefix
        // sum array only has an entry per mesh, not a sentinel.
        s.draw_lo = (s.mesh_lo < n) ? proxy_first_draw[s.mesh_lo] : total_draws;
        s.draw_hi = (s.mesh_hi < n) ? proxy_first_draw[s.mesh_hi] : total_draws;
    }
}

}  // namespace cairns
