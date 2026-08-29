// src/scene/transform_math.hpp
//
// PURE TRS composition, glm-only -- no entt, no Scene. The component-facing
// ComposeTRS(const Transform&) in transform_propagation.hpp forwards here, so
// the math the scene layer relies on (instance placement, hot-reload respawn
// positions, hierarchy compose) is testable without pulling the ECS into the
// spec target.

#ifndef CAIRNS_SCENE_TRANSFORM_MATH_HPP
#define CAIRNS_SCENE_TRANSFORM_MATH_HPP

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace cairns {

inline glm::mat4 ComposeTRS(const glm::vec3& t, const glm::quat& r,
                            const glm::vec3& s) {
    glm::mat4 m = glm::mat4_cast(r);
    m[0] *= s.x;
    m[1] *= s.y;
    m[2] *= s.z;
    m[3] = glm::vec4(t, 1.0f);
    return m;
}

// world of a child = parent's world * child's local. The composition rule
// PropagateTransforms walks shallow->deep.
inline glm::mat4 ComposeChild(const glm::mat4& parent_world,
                              const glm::vec3& t, const glm::quat& r,
                              const glm::vec3& s) {
    return parent_world * ComposeTRS(t, r, s);
}

}  // namespace cairns

#endif  // CAIRNS_SCENE_TRANSFORM_MATH_HPP
