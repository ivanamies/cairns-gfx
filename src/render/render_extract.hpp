#pragma once

#include "render/render_proxy_arrays.hpp"
#include "scene/scene_world.hpp"

#include <cassert>
#include <cstdint>

namespace cairns {

// Walk the live entities and produce render proxies. With `filter` empty (the
// default) iterates the packed live list; with a filter, iterates exactly the
// supplied handles (the parallel-test subset path). `root_override` lets callers
// frame against a different root than `world.root_transform`.
inline void Extract(SceneWorld& world, RenderProxyArrays& out,
                    std::span<const rhi::Handle<SceneEntity>> filter = {},
                    const glm::mat4* root_override = nullptr) {
    out.Clear();
    // Fixed-size scratch. Observed high-water across 100x33 (hw=59) and 50x66
    // (hw=49) benches; 256 = 4x headroom. 1 KB on the stack, zero heap.
    constexpr uint32_t kStackCap = 256;
    int32_t stack[kStackCap];
    uint32_t top;
    for (const SceneEntity& entity : world.entities) {
        if (entity.scene_index >= world.scene_count) {
            continue;
        }
        const Scene& scene = world.scenes[hot->scene_index];
        const glm::mat4 model_matrix = hot->transform * root;

        top = 0;
        for (size_t j = 0; j < scene.rootNodes.size(); ++j) {
            assert(top < kStackCap);
            stack[top++] = scene.rootNodes[j];
        }
        while (top > 0) {
            const int32_t node_idx = stack[--top];
            const Node& node = scene.nodes[node_idx];
            if (node.meshIndex < 0) {
                for (int32_t c : node.children) {
                    assert(top < kStackCap);
                    stack[top++] = c;
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
                assert(top < kStackCap);
                stack[top++] = c;
            }
        }
    }
}

}  // namespace cairns
