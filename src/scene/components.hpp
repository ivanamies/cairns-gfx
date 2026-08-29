// scene/components.hpp
//
// EnTT components attached to per-world registries. POD, small, no
// virtuals. The Extract view (render_extract.hpp) reads WorldTransform
// + AssetRef + Renderable; everything else is editor-only or wired
// later. Skin component is deferred to P8 (after the audit of the
// existing skin_output_pool / RangePool / RenderProxyArrays::skins
// machinery).

#pragma once

#include "scene/asset_registry.hpp"

#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <string>

namespace cairns {

// Authored: position + rotation + scale. Composed into a 4x4 by the
// transform propagation pass (P7).
struct Transform {
    glm::vec3 t{0.0f};
    glm::quat r{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 s{1.0f};
};

// Output of transform propagation; Extract reads THIS, not Transform.
// In P4 (pre-hierarchy) this is populated directly at world build.
struct WorldTransform {
    glm::mat4 world{1.0f};
};

// HANDLE into the shared AssetRegistry. NEVER a pointer.
struct AssetRef {
    AssetId asset;
};

// Visibility + layer membership. Layer_mask is a bitmask the View
// intersects against.
struct Renderable {
    uint32_t layer_mask = 0xFFFFFFFFu;
    uint32_t flags = 0;
};

// Intra-world hierarchy edge. Bare entt::entity is OK here per the
// STYLE.md anti-singleton rules -- the world is already named by
// virtue of which registry stores this Parent component. P7 wires it.
struct Parent {
    entt::entity value = entt::null;
};

// Tag: this entity (and its subtree, when P7 lands) needs transform
// propagation this frame. Empty struct -- entt uses presence/absence.
struct DirtyTransform {};

// Editor-only. NEVER read inside the Extract view (the Extract
// signature explicitly does not get the Name component).
struct Name {
    std::string value;
};

}  // namespace cairns
