// scene/transform_propagation.hpp
//
// PropagateTransforms walks a scene's hierarchy shallow->deep and
// populates WorldTransform for every entity that has a Transform.
// Roots compose root * compose(Transform); children compose
// parent.WorldTransform * compose(Transform). Recompute-all per dirty
// scene; DirtyTransform subtree-only pruning is not wired yet.
//
// No-op for entities that don't have a Transform component -- the
// scene-load path emplaces WorldTransform directly on each entity
// (the leaf-instance matrix) without Transform, and Extract reads
// WorldTransform * root_transform.

#pragma once

#include "scene/components.hpp"
#include "scene/world.hpp"

#include <cmath>

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

// Per-entity procedural animation: rewrite each animated entity's Transform
// from its captured rest pose + sim time `t`, then dirty it so the following
// PropagateTransforms recomposes WorldTransform. Deterministic in sim time.
inline void AnimateTransforms(Scene::Cold& wc, float t) {
    constexpr float kTau = 6.28318530717958647692f;
    auto& reg = wc.registry;
    auto view = reg.view<Transform, const TransformAnim>();
    for (auto e : view) {
        Transform& tr = view.get<Transform>(e);
        const TransformAnim& a = view.get<const TransformAnim>(e);
        switch (a.mode) {
            case TransformAnim::Mode::kSpin:
            case TransformAnim::Mode::kTumble:
                tr.r = a.base_r *
                       glm::angleAxis(a.rate * t, glm::normalize(a.axis));
                break;
            case TransformAnim::Mode::kBob:
                tr.t = a.base_t +
                       glm::vec3(0.0f, a.amp * std::sin(a.rate * kTau * t), 0.0f);
                break;
            case TransformAnim::Mode::kPulse:
                tr.s = a.base_s *
                       (1.0f + a.amp * 0.5f * (1.0f + std::sin(a.rate * kTau * t)));
                break;
            case TransformAnim::Mode::kOrbit:
                tr.t = a.base_t +
                       a.amp * glm::vec3(std::cos(a.rate * kTau * t), 0.0f,
                                         std::sin(a.rate * kTau * t));
                break;
        }
        reg.emplace_or_replace<DirtyTransform>(e);
    }
}

inline void PropagateTransforms(Scene::Cold& wc, const glm::mat4& root) {
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

    // Children: shallow->deep via depth-bounded fixpoint. Depth-sorting
    // (Aaltonen "topo sort once, walk many times") is the upgrade once
    // DirtyTransform subtrees land.
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
