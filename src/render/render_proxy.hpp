#pragma once

#include "core/handle.hpp"  // #220 Step 1: cairns::Handle template
#include "rhi/resource_manager.hpp"
#include "util/cpu_pool.hpp"  // #221 Phase 3: PoolSlice for SkinnedAttachment

#include <glm/glm.hpp>

#include <cstdint>
#include <string>

namespace cairns {

// #220 Step 1: forward-decl so PrimitiveProxy::material_id can be a
// Handle<Material> without dragging in gltf_loader.hpp.
struct Material;
// #221 Phase 3: forward-decl for SkinnedAttachment::Hot::mesh handle.
struct Mesh;
// #221 Phase 3: PrefabId forward-pass (real def in asset_registry.hpp).
struct Prefab;
using PrefabId = Handle<Prefab>;

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
    cairns::Handle<Material> material_id;  // #220 Step 1 (was uint32_t)
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
        // #222 Phase H.5 finish: PoolSlice (16 B + Allocation metadata) →
        // 4 B slice_offset on Hot. BuildSkinFrame only reads .offset to
        // fill InstanceMeta. Allocation metadata + count move to Cold (no
        // destroy path today; matters once Free fires).
        uint32_t slice_offset = 0;
        uint32_t joint_count = 0;
        // Sim-time phase + speed for clip eval (TODO determinism).
        float time_offset = 0.0f;
        float time_scale = 1.0f;
        // Mesh the slice was sized for; kernel uses mesh.vert_count.
        cairns::Handle<Mesh> mesh;
        // #222 Phase H.5: cached at skin-create so BuildSkinFrame avoids
        // skins_.GetCold + prefabs_.GetHot + prefabs_.GetCold per actor
        // per frame. UINT32_MAX means scene not registered with anim_eval.
        uint32_t gpu_prefab_header_idx = UINT32_MAX;
        float gpu_clip_duration = 1.0f;
        // #222 Phase E.6: per-actor stream-0 alias of skin_output_pool_
        // pre-offset by (pool_base + slice.offset * 16). Skinned draws
        // set vertex_buffers[0] = pos_stream; retired the F5
        // Draw::pos_buffer_byte_offset side channel. Non-owning VIEW.
        rhi::Handle<rhi::Buffer> pos_stream = rhi::Handle<rhi::Buffer>::Null;
    };
    struct Cold {
        cairns::PrefabId scene;
        uint32_t skin_index = 0;
        // #222 Phase H.5: clip_index demoted; not read on the per-frame
        // GPU eval path (the scene header carries the channel/sampler
        // bounds). Kept for debug + future late-toggle.
        int32_t clip_index = -1;
        // #222 Phase H.5 finish: full PoolSlice (incl OffsetAllocator::
        // Allocation) lives here. Only read at destroy/Free; today there
        // is no destroy path so it's effectively a deathbed reference.
        cairns::PoolSlice slice;
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
