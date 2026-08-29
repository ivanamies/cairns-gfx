#pragma once

#include "render/render_proxy_arrays.hpp"
#include "scene/scene_world.hpp"

#include <cstdint>
#include <vector>

namespace cairns {

inline void Extract(const SceneWorld& world, RenderProxyArrays& out) {
    out.Clear();
    std::vector<int32_t> stack;
    for (const SceneEntity& entity : world.entities) {
        if (entity.scene_index >= world.scene_count) {
            continue;
        }
        const Scene& scene = world.scenes[entity.scene_index];
        const glm::mat4 model_matrix = entity.transform * world.root_transform;

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
                stack.push_back(c);
            }
        }
    }
}

}  // namespace cairns
