// scene/components.hpp
//
// EnTT components attached to per-world registries. POD, small, no
// virtuals. The Extract view (render_extract.hpp) reads WorldTransform
// + AssetRef + Renderable; everything else is editor-only or wired
// later.
//
// P8 SKIN AUDIT (2026-06-04, deferred -- no animation in tree yet):
//   The codebase already has:
//     - util/cpu_pool.hpp: RangePool + PoolSlice (OffsetAllocator-
//       backed) -- the canonical user is named "skin_output_pool" in
//       the doc comment. Right tool for the GPU output range
//       suballoc (joint matrices, skinned vertices).
//     - render/render_proxy.hpp: SkinnedAttachment (per-frame proxy:
//       Handle<Buffer> joint_matrices + joint_count).
//     - render/render_proxy_arrays.hpp: ProxyArray<SkinnedAttachment>
//       skins (per-frame).
//   What's NOT there:
//     - No engine member named skin_output_pool_ (infra built, not
//       instantiated).
//     - No skinned content loads, no skinning compute kernel.
//   When animation lands, both machineries are needed (NOT redundant):
//     - RangePool skin_output_pool_   <- output offsets, O(1) free
//     - ResourceManager<SkinnedAttachment> skins_  <- per-skin
//       generational descriptor: joint_count, debug name, inverse-
//       bind data, slice handle into the RangePool.
//   At that point, add Skin component here (SkinId = Handle<
//   SkinnedAttachment>), wire SkinningJob holding SkinId, and route
//   RunSkinning through skins_.GetHot() generation check (the
//   explicit handle case from the spec's motivation).

#pragma once

#include "core/handle.hpp"  // #221 Phase 3: Handle for SkinId.
#include "scene/asset_registry.hpp"

#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <numbers>
#include <string>

namespace cairns {

// #221 Skinning Phase 3: forward-decl so Skin can carry a SkinId without
// pulling render/render_proxy.hpp into the component header.
struct SkinnedAttachment;
using SkinId = Handle<SkinnedAttachment>;

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

// #221 Skinning Phase 3: per-entity Skin reference. Carries a SkinId
// into Engine::skins_ (ResourceManager<SkinnedAttachment>). The actor's
// per-frame palette + skin compute dispatch are driven by this id; the
// generational handle catches stale references when an actor despawns
// and a new actor reuses the slot.
//
// Named SkinRef (not Skin) to avoid colliding with the glTF Skin schema
// type in src/util/gltf_loader.hpp -- this component holds a SkinId
// into a pool whose Cold->scene + skin_index point back at one of those
// glTF skins. Audit comment above predates the disambiguation.
struct SkinRef {
    SkinId id;
};

// Camera role #2 (Camera as a placed entity, per the resizing+cameras
// plan): attach this + a WorldTransform to an entity to make it a
// camera that a Viewport can bind to. Pose comes from the entity's
// WorldTransform; intrinsics live here.
//
// Convention: camera looks down -Z in its local frame (right-hand
// system, matching the engine's fly-cam math). The view matrix is
// inverse(WorldTransform.world) once Engine resolves the binding.
//
// is_main: convenience flag for tooling -- the studio surface's
// Camera.main returns the first entity with is_main = true in the
// active world. Multiple is_main entities is a configuration error;
// the studio surface complains loudly (no implicit fallback per
// Refinement 1 of the resizing+cameras plan).
struct CameraComponent {
    float fov_y_rad = 90.0f * std::numbers::pi_v<float> / 180.0f;
    float near_z = 0.1f;
    float far_z = 100.0f;
    bool is_main = false;
};

}  // namespace cairns
