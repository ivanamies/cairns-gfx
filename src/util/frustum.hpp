// src/util/frustum.hpp
//
// PURE frustum cull math used by BuildSkinFrame's per-actor cull. No GPU,
// no entt, no engine -- glm in, bool/AABB out.
//
// Convention matches the engine verbatim: Gribb-Hartmann planes from the
// transpose of view_proj (row3 +/- rowK), normalized; AABB tested with the
// positive-vertex ("n-vertex") method; the degenerate bind-pose AABB sentinel
// (min > max, e.g. a mesh with no POSITION accessor) is treated as ALWAYS
// VISIBLE so we never cull something we failed to measure.

#ifndef CAIRNS_UTIL_FRUSTUM_HPP
#define CAIRNS_UTIL_FRUSTUM_HPP

#include <array>

#include <glm/glm.hpp>

namespace cairns {

// A plane stored as (n.xyz, d); a point p is inside iff dot(n,p) + d >= 0.
using FrustumPlanes = std::array<glm::vec4, 6>;  // L,R,B,T,N,F

inline FrustumPlanes ExtractFrustumPlanes(const glm::mat4& view_proj) {
    const glm::mat4 m = glm::transpose(view_proj);
    FrustumPlanes pl{
        m[3] + m[0],  // left
        m[3] - m[0],  // right
        m[3] + m[1],  // bottom
        m[3] - m[1],  // top
        m[3] + m[2],  // near (zero-to-one depth)
        m[3] - m[2],  // far
    };
    for (auto& p : pl) {
        const float L = glm::length(glm::vec3(p));
        if (L > 0.0f) p /= L;
    }
    return pl;
}

// True iff the world-space AABB is fully outside the frustum (safe to cull).
inline bool AabbOutsideFrustum(const FrustumPlanes& pl, const glm::vec3& mn,
                               const glm::vec3& mx) {
    for (const glm::vec4& plane : pl) {
        const glm::vec3 n(plane);
        const float d = plane.w;
        // positive vertex: the AABB corner farthest along +n.
        const glm::vec3 p(n.x >= 0.0f ? mx.x : mn.x,
                          n.y >= 0.0f ? mx.y : mn.y,
                          n.z >= 0.0f ? mx.z : mn.z);
        if (glm::dot(n, p) + d < 0.0f) {
            return true;  // outside this plane
        }
    }
    return false;
}

// World AABB of a (possibly degenerate) bind-pose AABB under `world`, padded by
// `pad` about its center (the engine uses 1.5x to cover animation motion).
// Degenerate input (min > max) yields a huge box => never culled.
struct Aabb { glm::vec3 min; glm::vec3 max; };

inline Aabb WorldAabb(const glm::vec3& local_min, const glm::vec3& local_max,
                      const glm::mat4& world, float pad = 1.5f) {
    if (local_min.x > local_max.x) {
        return Aabb{glm::vec3(-1.0e30f), glm::vec3(1.0e30f)};
    }
    const glm::vec3 c = 0.5f * (local_min + local_max);
    const glm::vec3 h = pad * 0.5f * (local_max - local_min);
    glm::vec3 wmin(1.0e30f);
    glm::vec3 wmax(-1.0e30f);
    for (int i = 0; i < 8; ++i) {
        const glm::vec3 corner(c.x + ((i & 1) ? h.x : -h.x),
                               c.y + ((i & 2) ? h.y : -h.y),
                               c.z + ((i & 4) ? h.z : -h.z));
        const glm::vec4 wc4 = world * glm::vec4(corner, 1.0f);
        const glm::vec3 wc(wc4 / wc4.w);
        wmin = glm::min(wmin, wc);
        wmax = glm::max(wmax, wc);
    }
    return Aabb{wmin, wmax};
}

}  // namespace cairns

#endif  // CAIRNS_UTIL_FRUSTUM_HPP
