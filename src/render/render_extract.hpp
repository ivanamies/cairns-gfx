#pragma once

#include "render/render_proxy_arrays.hpp"
#include "scene/asset_registry.hpp"
#include "scene/components.hpp"
#include "scene/world.hpp"
#include "util/gltf_loader.hpp"
#include "util/log.hpp"

#include <cassert>
#include <cstdint>
#include <cstdlib>

namespace cairns {

// EnTT-driven extract. Iterates entities that have
// WorldTransform + AssetRef + Renderable; resolves Asset::Cold->cpu_graph
// for the DFS; emits the same MeshProxy/PrimitiveProxy shape as the old
// Extract above. Sets proxy.skin = kInvalidSkin unconditionally
// (matches old extract; skinning lands at P8).
// #220 Steps 2+3: meshes and scenes are engine-owned pools. Pool
// references threaded through so the inner walk can resolve each
// Asset::cpu_graph (now PrefabId) to Prefab::Hot+Cold and each Scene
// mesh entry (MeshId) to Mesh::Hot.
inline void ExtractFromScene(Scene::Cold& wc, const glm::mat4& root,
                             AssetRegistry& assets,
                             cairns::ResourceManager<Prefab>& prefabs_pool,
                             cairns::ResourceManager<Mesh>& meshes_pool,
                             RenderProxyArrays& out, bool append) {
    // append=true unions multiple scenes into one proxy array (multi-scene
    // per-viewport draw fan-out); first_primitive/mesh indices stay relative
    // to the accumulating arrays so ranges carve cleanly.
    if (!append) {
        out.Clear();
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

    auto view = wc.registry.view<const WorldTransform, const AssetRef,
                                 const Renderable>();
    for (auto entity : view) {
        const WorldTransform& xf = view.get<const WorldTransform>(entity);
        const AssetRef& ref = view.get<const AssetRef>(entity);
        const Renderable& rdr = view.get<const Renderable>(entity);

        // #221 Phase 9a: per-entity SkinRef -> packed SkinId for the
        // skinned mesh nodes below. {generation,index} packed into the
        // uint32 proxy.skin field; kInvalidSkin (0xFFFFFFFF) means
        // "static draw path", matching the unskinned default.
        uint32_t packed_skin = kInvalidSkin;
        if (const SkinRef* sr = wc.registry.try_get<const SkinRef>(entity)) {
            if (!sr->id.IsNull()) {
                packed_skin =
                    (static_cast<uint32_t>(sr->id.generation) << 16) |
                    static_cast<uint32_t>(sr->id.index);
            }
        }

        Asset::Cold* ac = assets.Pool().GetCold(ref.asset);
        if (!ac || ac->cpu_graph.IsNull()) {
            continue;
        }
        // #220 Step 3: cpu_graph is a PrefabId; resolve to Hot+Cold.
        Prefab::Hot* shot = prefabs_pool.GetHot(ac->cpu_graph);
        Prefab::Cold* scold = prefabs_pool.GetCold(ac->cpu_graph);
        if (!shot || !scold) {
            continue;
        }
        const glm::mat4 model = xf.world * root;

        top = 0;
        for (size_t j = 0; j < shot->rootNodes.size(); ++j) {
            push_or_die(shot->rootNodes[j]);
        }
        while (top > 0) {
            const int32_t node_idx = stack[--top];
            const Node& node = scold->nodes[node_idx];
            if (node.meshIndex < 0) {
                // #212 explicit pointer+size iteration. RelWithDebInfo doesn't
            // inline std::vector<int32_t>::begin()/end() reliably -- shows
            // up as ~5% self-time in the profile.
            const int32_t* cp = node.children.data();
            const size_t cn = node.children.size();
            for (size_t ci = 0; ci < cn; ++ci) {
                    push_or_die(cp[ci]);
                }
                continue;
            }

            // #220 Step 2: scene.meshes is std::vector<MeshId>; resolve
            // to the engine's pool Hot record for the GPU handles.
            const cairns::Handle<Mesh> mid = shot->meshes[node.meshIndex];
            const Mesh::Hot* mhot = meshes_pool.GetHot(mid);
            MeshProxy proxy;
            proxy.world_matrix = node.globalTransform * model;
            proxy.pos = mhot->posHandle;
            proxy.attr = mhot->attrHandle;
            proxy.index = mhot->indexHandle;
            proxy.first_primitive = static_cast<uint32_t>(out.primitives.size());
            proxy.primitive_count = static_cast<uint32_t>(mhot->primitives.size());
            proxy.skin = (node.skinIndex >= 0) ? packed_skin : kInvalidSkin;
            proxy.layer_mask = rdr.layer_mask;
            proxy.flags = rdr.flags;
            // #207 +1 so the value 0 (the id_off clear color) means
            // "background" rather than entity index 0.
            proxy.entity_id =
                static_cast<uint32_t>(entt::to_integral(entity)) + 1u;
            for (const Primitive& prim : mhot->primitives) {
                PrimitiveProxy pp;
                pp.first_index = prim.firstIndex;
                pp.index_count = prim.indexCount;
                pp.vertex_offset = prim.vertexOffset;
                pp.material_id = shot->materials[prim.materialIndex];
                out.primitives.push_back(pp);
            }
            out.meshes.push_back(proxy);

            for (int32_t c : node.children) {
                push_or_die(c);
            }
        }
    }
}

}  // namespace cairns
