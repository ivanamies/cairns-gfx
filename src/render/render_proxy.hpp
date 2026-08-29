#pragma once

#include "rhi/resource_manager.hpp"

#include <glm/glm.hpp>

#include <cstdint>

namespace cairns {

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
};

struct PrimitiveProxy {
    uint32_t first_index = 0;
    uint32_t index_count = 0;
    int32_t vertex_offset = 0;
    uint32_t material_id = 0;
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

struct SkinnedAttachment {
    rhi::Handle<rhi::Buffer> joint_matrices;
    uint32_t joint_count = 0;
};

// Persistent light, owned by SceneWorld::lights (ResourceManager). The per-frame
// ProxyArray<LightProxy> in RenderProxyArrays stays as the transient "visible this
// frame" extract output -- those are computed each Extract from the persistent set.
struct LightProxy {
    struct Hot {
        glm::vec4 position = glm::vec4(0.0f);
        glm::vec4 direction = glm::vec4(0.0f);
        glm::vec4 color = glm::vec4(1.0f);
        float range = 0.0f;
        uint32_t layer_mask = 0xFFFFFFFFu;
        uint32_t flags = kProxyVisible | kProxyCastShadow;
    };
    struct Cold {
        const char* debug_name = nullptr;
    };

    // BACKWARDS-COMPAT for the per-frame ProxyArray<LightProxy>: the transient
    // visible-this-frame proxy still uses the flat field layout below for now.
    // When the persistent set actually gets lights, the Extract pass will pull
    // from SceneWorld::lights and emit into a renamed transient array.
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
