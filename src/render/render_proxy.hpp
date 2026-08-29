#pragma once

#include "core/handle.hpp"  // #220 Step 1: cairns::Handle template
#include "rhi/resource_manager.hpp"
#include "util/cpu_pool.hpp"  // #221 Phase 3: PoolSlice for SkinnedAttachment

#include <glm/glm.hpp>

#include <cstdint>
#include <string>

namespace cairns {

// #220 Step 1: forward-decl so PrimitiveProxy::material_id can be a
// Handle<LoadedMaterial> without dragging in gltf_loader.hpp.
struct LoadedMaterial;
// #221 Phase 3: forward-decl for SkinnedAttachment::Hot::mesh handle.
struct Mesh;
// #221 Phase 3: SceneId forward-pass (real def in asset_registry.hpp).
struct Scene;
using SceneId = Handle<Scene>;

static constexpr uint32_t kInvalidSkin = 0xFFFFFFFFu;

enum ProxyFlags : uint32_t {
    kProxyVisible = 1u << 0,
    kProxyCastShadow = 1u << 1,
    kProxyLit = 1u << 2,
};

struct MeshProxy {
    glm::mat4 world_matrix = glm::mat4(1.0f);
    rhi::Handle<rhi::Buffer> pos;
    rhi::Handle<rhi::Buffer> attr;
    rhi::Handle<rhi::Buffer> index;
    uint32_t first_primitive = 0;
    uint32_t primitive_count = 0;
    uint32_t skin = kInvalidSkin;
    uint32_t layer_mask = 0xFFFFFFFFu;
    uint32_t flags = kProxyVisible;
    // #207 entity id baked into the R32U id_off by unlit.frag. Stored as
    // entt::to_integral(entity) + 1 so the value 0 means "background" (the
    // forward pass clears id_off to 0 and the outline shader treats id==0
    // as the empty highlight). 0xFFFFFFFF reserved for "unknown".
    uint32_t entity_id = 0;
};

struct PrimitiveProxy {
    uint32_t first_index = 0;
    uint32_t index_count = 0;
    int32_t vertex_offset = 0;
    cairns::Handle<LoadedMaterial> material_id;  // #220 Step 1 (was uint32_t)
};

struct LineProxy {
    glm::vec3 a = glm::vec3(0.0f);
    glm::vec3 b = glm::vec3(0.0f);
    glm::vec4 color = glm::vec4(1.0f);
    uint32_t layer_mask = 0xFFFFFFFFu;
    uint32_t flags = kProxyVisible;
    uint32_t is_2d_overlay = 0;
};

struct PointProxy {
    glm::vec3 position = glm::vec3(0.0f);
    glm::vec4 color = glm::vec4(1.0f);
    float size = 1.0f;
    uint32_t layer_mask = 0xFFFFFFFFu;
    uint32_t flags = kProxyVisible;
    uint32_t is_2d_overlay = 0;
};

// #221 Skinning Phase 3: Aaltonen Hot/Cold split for SkinnedAttachment.
// Pooled via cairns::ResourceManager<SkinnedAttachment> on Engine; SkinId
// = Handle<SkinnedAttachment>. Hot is what the per-frame skin path reads
// every dispatch (slice + joint_count + clip ref + time); Cold carries
// the scene/skin-index resolution path used at palette eval to recover
// the joint set + inverse binds, plus a debug name.
//
// The slice is the per-actor slab in skin_output_pool_, measured in vec4
// (16 B) vertex units. Skinned draws point stream 0 at
// (pool_buffer, slice.offset * 16).
struct SkinnedAttachment {
    struct Hot {
        cairns::PoolSlice slice;
        uint32_t joint_count = 0;
        // Scene-local clip index (-1 = bind pose / no animation).
        int32_t clip_index = -1;
        // Sim-time phase + speed for clip eval (TODO determinism).
        float time_offset = 0.0f;
        float time_scale = 1.0f;
        // Mesh the slice was sized for; kernel uses mesh.vert_count.
        cairns::Handle<Mesh> mesh;
    };
    struct Cold {
        cairns::SceneId scene;
        uint32_t skin_index = 0;
        std::string name;
    };
};
using SkinId = Handle<SkinnedAttachment>;

struct LightProxy {
    glm::vec4 position = glm::vec4(0.0f);
    glm::vec4 direction = glm::vec4(0.0f);
    glm::vec4 color = glm::vec4(1.0f);
    float range = 0.0f;
    uint32_t layer_mask = 0xFFFFFFFFu;
    uint32_t flags = kProxyVisible | kProxyCastShadow;
};

struct CameraProxy {
    glm::mat4 view = glm::mat4(1.0f);
    glm::mat4 proj = glm::mat4(1.0f);
    glm::vec4 position = glm::vec4(0.0f);
    uint32_t layer_mask = 0xFFFFFFFFu;
};

struct LayerProxy {
    uint32_t layer_mask = 0;
    uint32_t flags = 0;
};

}  // namespace cairns
