// scene/transform_propagation.hpp
//
// PropagateTransforms walks a world's hierarchy shallow->deep and
// populates WorldTransform for every entity that has a Transform.
// Roots compose root * compose(Transform); children compose
// parent.WorldTransform * compose(Transform). v1 recompute-all per
// dirty world; DirtyTransform subtree-only is the v2 optimization.
//
// No-op for entities that don't have a Transform component -- the
// current scene-load path emplaces WorldTransform directly on each
// entity (the leaf-instance matrix) without Transform, and Extract
// reads WorldTransform * root_transform. P8+ wires authored TRS via
// Transform with this propagation as the bridge.

#pragma once

#include "scene/components.hpp"
#include "scene/world.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

namespace cairns {

inline glm::mat4 ComposeTRS(const Transform& t) {
    glm::mat4 m = glm::mat4_cast(t.r);
    m[0] *= t.s.x;
    m[1] *= t.s.y;
    m[2] *= t.s.z;
    m[3] = glm::vec4(t.t, 1.0f);
    return m;
}

inline void PropagateTransforms(World::Cold& wc, const glm::mat4& root) {
    auto& reg = wc.registry;

    // Roots: entities with Transform but no Parent.
    auto root_view = reg.view<const Transform>(entt::exclude<Parent>);
    for (auto e : root_view) {
        const Transform& t = root_view.get<const Transform>(e);
        const glm::mat4 world = root * ComposeTRS(t);
        if (auto* wt = reg.try_get<WorldTransform>(e)) {
            wt->world = world;
        } else {
            reg.emplace<WorldTransform>(e, WorldTransform{world});
        }
    }

    // Children: shallow->deep via depth-bounded fixpoint. v2 will
    // sort by depth (Aaltonen "topo sort once, walk many times")
    // when DirtyTransform subtrees land.
    auto child_view = reg.view<const Transform, const Parent>();
    bool changed = true;
    int iter = 0;
    constexpr int kMaxIters = 32;  // depth ceiling; abort if exceeded.
    while (changed && iter < kMaxIters) {
        changed = false;
        ++iter;
        for (auto e : child_view) {
            const Parent& p = child_view.get<const Parent>(e);
            if (p.value == entt::null) {
                continue;
            }
            auto* parent_wt = reg.try_get<WorldTransform>(p.value);
            if (!parent_wt) {
                // Parent not yet propagated this pass; try next.
                continue;
            }
            const Transform& t = child_view.get<const Transform>(e);
            const glm::mat4 world = parent_wt->world * ComposeTRS(t);
            if (auto* wt = reg.try_get<WorldTransform>(e)) {
                if (wt->world == world) {
                    continue;
                }
                wt->world = world;
            } else {
                reg.emplace<WorldTransform>(e, WorldTransform{world});
            }
            changed = true;
        }
    }
}

}  // namespace cairns
