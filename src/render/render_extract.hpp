#pragma once

#include "render/render_proxy_arrays.hpp"
#include "scene/asset_registry.hpp"
#include "scene/components.hpp"
#include "scene/scene_world.hpp"
#include "scene/world.hpp"
#include "util/gltf_loader.hpp"
#include "util/log.hpp"

#include <cassert>
#include <cstdint>
#include <cstdlib>

namespace cairns {

inline void Extract(const SceneWorld& world, RenderProxyArrays& out) {
    out.Clear();
    // Fixed-size scratch. Observed high-water across 100x33 (hw=59) and 50x66
    // (hw=49) benches; 256 = 4x headroom. 1 KB on the stack, zero heap.
    constexpr uint32_t kStackCap = 256;
    int32_t stack[kStackCap];
    uint32_t top;
    auto push_or_die = [&](int32_t v) {
        if (top >= kStackCap) {
            CAIRNS_PRINT(
                "render_extract: scratch stack overflow (top=%u, cap=%u)\n",
                top, kStackCap);
            std::abort();
        }
        stack[top++] = v;
    };
    for (const SceneEntity& entity : world.entities) {
        if (entity.scene_index >= world.scene_count) {
            continue;
        }
        const Scene& scene = world.scenes[entity.scene_index];
        const glm::mat4 model_matrix = entity.transform * world.root_transform;

        top = 0;
        for (size_t j = 0; j < scene.rootNodes.size(); ++j) {
            push_or_die(scene.rootNodes[j]);
        }
        while (top > 0) {
            const int32_t node_idx = stack[--top];
            const Node& node = scene.nodes[node_idx];
            if (node.meshIndex < 0) {
                for (int32_t c : node.children) {
                    push_or_die(c);
                }
                continue;
            }

            const Mesh& mesh = scene.meshes[node.meshIndex];
            MeshProxy proxy;
            proxy.world_matrix = node.globalTransform * model_matrix;
            proxy.pos = mesh.posHandle;
            proxy.attr = mesh.attrHandle;
            proxy.index = mesh.indexHandle;
            proxy.first_primitive = static_cast<uint32_t>(out.primitives.size());
            proxy.primitive_count = static_cast<uint32_t>(mesh.primitives.size());
            proxy.skin = kInvalidSkin;
            proxy.layer_mask = entity.layer_mask;
            proxy.flags = entity.flags;
            for (const Primitive& prim : mesh.primitives) {
                PrimitiveProxy pp;
                pp.first_index = prim.firstIndex;
                pp.index_count = prim.indexCount;
                pp.vertex_offset = prim.vertexOffset;
                pp.material_id = scene.materialIds[prim.materialIndex];
                out.primitives.push_back(pp);
            }
            out.meshes.Add(proxy);

            for (int32_t c : node.children) {
                push_or_die(c);
            }
        }
    }
}

// EnTT-driven extract (P4 parity path). Iterates entities that have
// WorldTransform + AssetRef + Renderable; resolves Asset::Cold->cpu_graph
// for the DFS; emits the same MeshProxy/PrimitiveProxy shape as the old
// Extract above. Sets proxy.skin = kInvalidSkin unconditionally
// (matches old extract; skinning lands at P8).
inline void ExtractFromWorld(World::Cold& wc, const glm::mat4& root,
                             AssetRegistry& assets, RenderProxyArrays& out) {
    out.Clear();
    if (!wc.registry) {
        return;
    }
    constexpr uint32_t kStackCap = 256;
    int32_t stack[kStackCap];
    uint32_t top;
    auto push_or_die = [&](int32_t v) {
        if (top >= kStackCap) {
            CAIRNS_PRINT(
                "render_extract: scratch stack overflow (top=%u, cap=%u)\n",
                top, kStackCap);
            std::abort();
        }
        stack[top++] = v;
    };

    auto view = wc.registry->view<const WorldTransform, const AssetRef,
                                  const Renderable>();
    for (auto entity : view) {
        const WorldTransform& xf = view.get<const WorldTransform>(entity);
        const AssetRef& ref = view.get<const AssetRef>(entity);
        const Renderable& rdr = view.get<const Renderable>(entity);

        Asset::Cold* ac = assets.Pool().GetCold(ref.asset);
        if (!ac || !ac->cpu_graph) {
            continue;
        }
        const Scene& scene = *ac->cpu_graph;
        const glm::mat4 model = xf.world * root;

        top = 0;
        for (size_t j = 0; j < scene.rootNodes.size(); ++j) {
            push_or_die(scene.rootNodes[j]);
        }
        while (top > 0) {
            const int32_t node_idx = stack[--top];
            const Node& node = scene.nodes[node_idx];
            if (node.meshIndex < 0) {
                for (int32_t c : node.children) {
                    push_or_die(c);
                }
                continue;
            }

            const Mesh& mesh = scene.meshes[node.meshIndex];
            MeshProxy proxy;
            proxy.world_matrix = node.globalTransform * model;
            proxy.pos = mesh.posHandle;
            proxy.attr = mesh.attrHandle;
            proxy.index = mesh.indexHandle;
            proxy.first_primitive = static_cast<uint32_t>(out.primitives.size());
            proxy.primitive_count = static_cast<uint32_t>(mesh.primitives.size());
            proxy.skin = kInvalidSkin;
            proxy.layer_mask = rdr.layer_mask;
            proxy.flags = rdr.flags;
            for (const Primitive& prim : mesh.primitives) {
                PrimitiveProxy pp;
                pp.first_index = prim.firstIndex;
                pp.index_count = prim.indexCount;
                pp.vertex_offset = prim.vertexOffset;
                pp.material_id = scene.materialIds[prim.materialIndex];
                out.primitives.push_back(pp);
            }
            out.meshes.Add(proxy);

            for (int32_t c : node.children) {
                push_or_die(c);
            }
        }
    }
}

}  // namespace cairns
