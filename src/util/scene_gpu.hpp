#pragma once

#include "util/gltf_loader.hpp"
#include "rhi/resource_manager.hpp"
#include "rhi/resources.hpp"

#include <cstring>
#include <span>
#include <vector>

namespace cairns::rhi {

inline bool LoadScenesGpu(std::span<Scene> scenes, Resources& rm, Allocator& alloc) {
    // Pack EVERY loaded GLB's vertices in lockstep into ONE shared buffer set: one
    // position buffer and one attribute buffer holding all meshes of all scenes in
    // the same order (vertex-aligned, not interleaved), plus one shared index
    // buffer. Each mesh shares these three handles and selects its primitives via
    // an all-GLBs-global base vertex / base index. Because every draw in the frame
    // resolves to the same buffers, the change-tracked recorder binds each stream
    // exactly once per frame (Aaltonen "Pack Meshes": baseVertex/baseIndex in the
    // draw, binds on change).
    size_t total_verts = 0;
    size_t total_indices = 0;
    for (const Scene& scene : scenes) {
        for (const Mesh& mesh : scene.meshes) {
            total_verts += mesh.cpuPositions.size();
            total_indices += mesh.cpuIndices.size();
        }
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
    for (Scene& scene : scenes) {
        for (Mesh& mesh : scene.meshes) {
            const int32_t base_vertex = static_cast<int32_t>(pos_all.size());
            const uint32_t base_index = static_cast<uint32_t>(idx_all.size());
            pos_all.insert(pos_all.end(), mesh.cpuPositions.begin(), mesh.cpuPositions.end());
            attr_all.insert(attr_all.end(), mesh.cpuAttrs.begin(), mesh.cpuAttrs.end());
            idx_all.insert(idx_all.end(), mesh.cpuIndices.begin(), mesh.cpuIndices.end());
            for (Primitive& prim : mesh.primitives) {
                prim.vertexOffset += base_vertex;  // mesh-local -> all-GLBs-global
                prim.firstIndex += base_index;
            }
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

    // One physical vertex buffer: [ positions (N*16) | attributes (N*64) ], the two
    // sections "next to each other" (not interleaved) for cache locality. The
    // position stream binds the buffer base; the attribute stream binds the same
    // buffer via a non-owning alias handle at the attribute-section offset.
    const size_t pos_bytes = pos_all.size() * sizeof(glm::vec4);
    const size_t attr_bytes = attr_all.size() * sizeof(VertexAttribute);
    std::vector<uint8_t> vbytes(pos_bytes + attr_bytes);
    std::memcpy(vbytes.data(), pos_all.data(), pos_bytes);
    std::memcpy(vbytes.data() + pos_bytes, attr_all.data(), attr_bytes);

    Handle<Buffer> shared_vtx;
    Handle<Buffer> shared_idx;
    if (!make(shared_vtx, vbytes.data(), vbytes.size())) {
        return false;
    }
    if (!make(shared_idx, idx_all.data(), idx_all.size() * sizeof(uint32_t))) {
        return false;
    }

    // Non-owning alias into shared_vtx at the attribute section. shared_vtx owns the
    // allocation; this handle's Cold is left default so it is never freed.
    Handle<Buffer> attr_alias = rm.buffers.Acquire();
    {
        Buffer::Hot* vh = rm.buffers.GetHot(shared_vtx);
        Buffer::Hot* ah = rm.buffers.GetHot(attr_alias);
        ah->heap_buffer_index = vh->heap_buffer_index;
        ah->offset_in_heap = vh->offset_in_heap + static_cast<uint32_t>(pos_bytes);
    }

    for (Scene& scene : scenes) {
        for (Mesh& mesh : scene.meshes) {
            mesh.posHandle = shared_vtx;
            mesh.attrHandle = attr_alias;
            mesh.indexHandle = shared_idx;
        }
    }
    return true;
}

}  // namespace cairns::rhi
