#pragma once

#include "util/gltf_loader.hpp"
#include "rhi/resource_manager.hpp"
#include "rhi/resources.hpp"

namespace cairns::rhi {

inline bool LoadSceneGpu(Scene& scene, Resources& rm, Allocator& alloc) {
    // Pack the whole scene's vertices in lockstep: one shared position buffer and
    // one shared attribute buffer, each holding EVERY mesh's vertices in the same
    // order (vertex-aligned, not interleaved), plus one shared index buffer. Each
    // mesh then shares these three handles and selects its primitives via a
    // scene-global base vertex / base index. Because every mesh resolves to the
    // same buffers, the change-tracked recorder binds each stream once per scene
    // (Aaltonen "Pack Meshes": baseVertex/baseIndex in the draw, binds on change).
    size_t total_verts = 0;
    size_t total_indices = 0;
    for (const Mesh& mesh : scene.meshes) {
        total_verts += mesh.cpuPositions.size();
        total_indices += mesh.cpuIndices.size();
    }
    if (total_verts == 0) {
        return true;
    }

    std::vector<glm::vec4> pos_all;
    pos_all.reserve(total_verts);
    std::vector<VertexAttribute> attr_all;
    attr_all.reserve(total_verts);
    std::vector<uint32_t> idx_all;
    idx_all.reserve(total_indices);
    for (Mesh& mesh : scene.meshes) {
        const int32_t base_vertex = static_cast<int32_t>(pos_all.size());
        const uint32_t base_index = static_cast<uint32_t>(idx_all.size());
        pos_all.insert(pos_all.end(), mesh.cpuPositions.begin(), mesh.cpuPositions.end());
        attr_all.insert(attr_all.end(), mesh.cpuAttrs.begin(), mesh.cpuAttrs.end());
        idx_all.insert(idx_all.end(), mesh.cpuIndices.begin(), mesh.cpuIndices.end());
        for (Primitive& prim : mesh.primitives) {
            prim.vertexOffset += base_vertex;  // mesh-local -> scene-global
            prim.firstIndex += base_index;
        }
    }

    auto make = [&](Handle<Buffer>& out, const void* data, size_t bytes) -> bool {
        BufferDesc d;
        d.byte_size = static_cast<uint32_t>(bytes);
        d.usage = kUsageVertex | kUsageIndex;
        d.memory = Memory::kDefault;
        d.initial_data = std::span<const uint8_t>(
            static_cast<const uint8_t*>(data), bytes);
        out = rm.CreateBuffer(alloc, d);
        return !out.IsNull();
    };
    Handle<Buffer> shared_pos;
    Handle<Buffer> shared_attr;
    Handle<Buffer> shared_idx;
    if (!make(shared_pos, pos_all.data(), pos_all.size() * sizeof(glm::vec4))) {
        return false;
    }
    if (!make(shared_attr, attr_all.data(), attr_all.size() * sizeof(VertexAttribute))) {
        return false;
    }
    if (!make(shared_idx, idx_all.data(), idx_all.size() * sizeof(uint32_t))) {
        return false;
    }
    for (Mesh& mesh : scene.meshes) {
        mesh.posHandle = shared_pos;
        mesh.attrHandle = shared_attr;
        mesh.indexHandle = shared_idx;
    }
    return true;
}

}  // namespace cairns::rhi
