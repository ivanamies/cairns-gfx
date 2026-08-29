// scene/transform_propagation.hpp
//
// PropagateTransforms walks a world's hierarchy shallow->deep and
// populates WorldTransform for every entity that has a Transform.
// Roots compose root * compose(Transform); children compose
// parent.WorldTransform * compose(Transform). v1 recompute-all per
// dirty world; DirtyTransform subtree-only is the v2 optimization.
//
// Empty in P3; P7 lands the body. Declared here so engine.hpp can take
// its address from P4 onward without forward-declaration churn.

#pragma once

#include "scene/world.hpp"

#include <glm/glm.hpp>

namespace cairns {

inline void PropagateTransforms(World::Cold& wc, const glm::mat4& root) {
    (void)wc;
    (void)root;
    // P7: walk Parent edges shallow->deep; compose into WorldTransform.
}

}  // namespace cairns
