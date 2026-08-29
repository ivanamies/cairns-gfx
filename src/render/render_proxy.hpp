#pragma once

#include "core/handle.hpp"
#include "rhi/resource_manager.hpp"
#include "util/cpu_pool.hpp"  // PoolSlice for SkinnedAttachment::Cold.

#include <glm/glm.hpp>

#include <cstdint>
#include <string>

namespace cairns {

// Forward-decls so proxies can hold handles without dragging in
// gltf_loader.hpp / asset_registry.hpp (which define the real types).
struct Material;
struct Mesh;
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
    // Entity id baked into the R32U id_off by unlit.frag. Stored as
    // entt::to_integral(entity) + 1 so 0 means "background" (the forward
    // pass clears id_off to 0; the outline shader treats id==0 as the
    // empty highlight). 0xFFFFFFFF reserved for "unknown".
    uint32_t entity_id = 0;
};

struct PrimitiveProxy {
    uint32_t first_index = 0;
    uint32_t index_count = 0;
    int32_t vertex_offset = 0;
    cairns::Handle<Material> material_id;
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

// Aaltonen Hot/Cold split. Pooled via ResourceManager<SkinnedAttachment>
// on Engine; SkinId = Handle<SkinnedAttachment>. Hot is what the
// per-frame skin path reads every dispatch (slice + joint_count + clip
// ref + time); Cold carries the scene/skin-index resolution path used at
// palette eval to recover the joint set + inverse binds, plus a debug
// name.
//
// The slice is the per-actor slab in skin_output_pool_, measured in vec4
// (16 B) vertex units. Skinned draws point stream 0 at
// (pool_buffer, slice.offset * 16).
struct SkinnedAttachment {
    struct Hot {
        // Only the slice offset is per-frame hot (BuildSkinFrame reads it
        // to fill InstanceMeta); the full PoolSlice lives in Cold.
        uint32_t slice_offset = 0;
        uint32_t joint_count = 0;
        // Sim-time phase + speed for clip eval.
        float time_offset = 0.0f;
        float time_scale = 1.0f;
        // Mesh the slice was sized for; kernel uses mesh.vert_count.
        cairns::Handle<Mesh> mesh;
        // Cached at skin-create so BuildSkinFrame avoids skins_.GetCold +
        // prefabs_.GetHot + prefabs_.GetCold per actor per frame.
        // UINT32_MAX means scene not registered with anim_eval.
        uint32_t gpu_prefab_header_idx = UINT32_MAX;
        float gpu_clip_duration = 1.0f;
        // Per-actor stream-0 alias of skin_output_pool_, pre-offset by
        // (pool_base + slice.offset * 16). Skinned draws set
        // vertex_buffers[0] = pos_stream. Non-owning view.
        rhi::Handle<rhi::Buffer> pos_stream = rhi::Handle<rhi::Buffer>::Null;
    };
    struct Cold {
        cairns::PrefabId scene;
        uint32_t skin_index = 0;
        // Not read on the per-frame GPU eval path (the scene header
        // carries the channel/sampler bounds). Kept for debug + a future
        // late clip toggle.
        int32_t clip_index = -1;
        // Full PoolSlice (incl. OffsetAllocator::Allocation). Only read
        // at destroy/Free.
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
