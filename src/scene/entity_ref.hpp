// scene/entity_ref.hpp
//
// Cross-boundary entity reference: SceneId + entt::entity. Per the
// STYLE.md "Anti-singleton (scene layer)" rules, a bare entt::entity in
// a signature/member/container is a defect -- every cross-system entity
// reference goes through EntityRef so the (world, entity) pair travels
// together. EnTT versions entities, so EntityRef is dangling-safe on
// both axes (Handle<Scene> generation + entt::entity version).

#pragma once

#include "scene/world.hpp"

#include <entt/entt.hpp>

namespace cairns {

struct EntityRef {
    SceneId scene;
    entt::entity entity = entt::null;

    bool valid() const {
        return !scene.IsNull() && entity != entt::null;
    }
};

}  // namespace cairns
