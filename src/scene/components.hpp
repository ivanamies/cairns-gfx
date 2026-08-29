// scene/components.hpp
//
// EnTT components attached to per-scene registries. POD, small, no
// virtuals. Components are the product's extension surface (Unity
// vocabulary): a feature is a component a scene spawns + a system
// iterates, never an engine flag. The Extract view (render_extract.hpp)
// reads WorldTransform + AssetRef + Renderable; everything else is
// editor-only.

#pragma once

#include "core/handle.hpp"  // Handle for SkinId.
#include "scene/asset_registry.hpp"

#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <numbers>
#include <string>

namespace cairns {

// Forward-decl so SkinRef can carry a SkinId without pulling
// render/render_proxy.hpp into the component header.
struct SkinnedAttachment;
using SkinId = Handle<SkinnedAttachment>;

// Authored: position + rotation + scale. Composed into a 4x4 by the
// transform propagation pass.
struct Transform {
    glm::vec3 t{0.0f};
    glm::quat r{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 s{1.0f};
};

// Per-entity procedural animation, applied each frame before propagation
// (AnimateTransforms). Rewrites Transform from a captured rest pose (base_*)
// plus sim time -- so motion is a per-entity component, NOT a global scene
// spin.
struct TransformAnim {
    enum class Mode : uint8_t { kSpin, kTumble, kBob, kPulse, kOrbit };
    Mode mode = Mode::kSpin;
    glm::vec3 axis{0.0f, 1.0f, 0.0f};  // spin/tumble rotation axis
    float rate = 1.0f;   // spin/tumble rad/s; bob/pulse/orbit cycles/s
    float amp = 1.0f;    // bob height / pulse depth / orbit radius (world units)
    glm::vec3 base_t{0.0f};
    glm::quat base_r{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 base_s{1.0f};
};

// Output of transform propagation; Extract reads THIS, not Transform.
// The scene-load path may also populate it directly (leaf-instance
// matrices, no Transform component).
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

// Intra-scene hierarchy edge. Bare entt::entity is OK here per the
// STYLE.md anti-singleton rules -- the scene is already named by
// virtue of which registry stores this Parent component.
struct Parent {
    entt::entity value = entt::null;
};

// Tag: this entity needs transform propagation this frame. Empty
// struct -- entt uses presence/absence.
struct DirtyTransform {};

// Editor-only. NEVER read inside the Extract view (the Extract
// signature explicitly does not get the Name component).
struct Name {
    std::string value;
};

// Per-entity Skin reference. Carries a SkinId into Engine::skins_
// (ResourceManager<SkinnedAttachment>). The actor's per-frame palette +
// skin compute dispatch are driven by this id; the generational handle
// catches stale references when an actor despawns and a new actor
// reuses the slot.
//
// Named SkinRef (not Skin) to avoid colliding with the glTF Skin schema
// type in src/util/gltf_loader.hpp -- this component holds a SkinId
// into a pool whose Cold->scene + skin_index point back at one of those
// glTF skins.
struct SkinRef {
    SkinId id;
};

// Camera role #2 (Camera as a placed entity): attach this + a
// WorldTransform to an entity to make it a camera that a Viewport can
// bind to. Pose comes from the entity's WorldTransform; intrinsics live
// here.
//
// Convention: camera looks down -Z in its local frame (right-hand
// system, matching the engine's fly-cam math). The view matrix is
// inverse(WorldTransform.world) once Engine resolves the binding.
//
// is_main: convenience flag for tooling -- the studio surface's
// Camera.main returns the first entity with is_main = true in the
// active scene. Multiple is_main entities is a configuration error;
// the studio surface complains loudly (no implicit fallback).
struct CameraComponent {
    float fov_y_rad = 90.0f * std::numbers::pi_v<float> / 180.0f;
    float near_z = 0.1f;
    float far_z = 100.0f;
    bool is_main = false;
};

// Presence in a scene's registry gates the particle compute + draw -- a
// component, not an engine flag. Per-scene, so [N-node] node K can emit
// while node J doesn't. Empty tag today -- the sim reads its params from
// the engine-global ParticleSystem; a future per-emitter config adds
// render::EmitterParams here once the kernel consumes them.
struct ParticleEmitterComponent {};

}  // namespace cairns
