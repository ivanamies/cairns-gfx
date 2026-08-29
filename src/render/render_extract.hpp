#pragma once

#include "render/render_proxy_arrays.hpp"
#include "scene/scene_world.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace cairns {

// Walk the live entities and produce render proxies. With `filter` empty (the
// default) iterates the packed live list; with a filter, iterates exactly the
// supplied handles (the parallel-test subset path). `root_override` lets callers
// frame against a different root than `world.root_transform`.
inline void Extract(SceneWorld& world, RenderProxyArrays& out,
                    std::span<const rhi::Handle<SceneEntity>> filter = {},
                    const glm::mat4* root_override = nullptr) {
    out.Clear();
    const std::span<const rhi::Handle<SceneEntity>> handles =
        filter.empty()
            ? std::span<const rhi::Handle<SceneEntity>>(world.live_entities.data(),
                                                        world.live_entities.size())
            : filter;
    const glm::mat4& root =
        root_override != nullptr ? *root_override : world.root_transform;
    std::vector<int32_t> stack;
    for (rhi::Handle<SceneEntity> h : handles) {
        SceneEntity::Hot* hot = world.entities.GetHot(h);
        if (hot == nullptr) {
            continue;  // stale handle -- silently skip (D's stale-detection)
        }
        if (hot->scene_index >= world.scene_count) {
            continue;
        }
        const Scene& scene = world.scenes[hot->scene_index];
        const glm::mat4 model_matrix = hot->transform * root;

        stack.clear();
        for (size_t j = 0; j < scene.rootNodes.size(); ++j) {
            stack.push_back(scene.rootNodes[j]);
        }
        while (!stack.empty()) {
            const int32_t node_idx = stack.back();
            stack.pop_back();
            const Node& node = scene.nodes[node_idx];
            if (node.meshIndex < 0) {
                for (int32_t c : node.children) {
                    stack.push_back(c);
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
            proxy.layer_mask = hot->layer_mask;
            proxy.flags = hot->flags;
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
                stack.push_back(c);
            }
        }
    }
}

}  // namespace cairns
