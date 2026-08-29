#pragma once

#include "util/gltf_loader.hpp"
#include "rhi/resource_manager.hpp"
#include "rhi/resources.hpp"

#include <cstring>
#include <span>
#include <vector>

namespace cairns::rhi {

// Batched upload: stage K scenes' worth of vertex/index data at a time, free
// the CPU temporaries between batches, then run the next batch. This bounds
// the CPU working set at ~K * per_glb_bytes (instead of all-glbs at once) and
// gives the upload bump ring a chance to recycle. The final on-GPU layout is
// unchanged: ONE shared vertex buffer [positions | attributes] + ONE shared
// index buffer, with primitives' vertexOffset / firstIndex patched to global.
// #220 Step 3: scenes are engine-owned via cairns::ResourceManager<Scene>;
// caller passes a span of SceneIds + the pool. Mesh pool also threaded
// (Step 2 invariant).
inline bool LoadScenesGpu(std::span<const cairns::SceneId> scene_ids,
                          cairns::ResourceManager<Scene>& scenes_pool,
                          cairns::ResourceManager<Mesh>& meshes_pool,
                          Resources& rm, Allocator& alloc) {
    static constexpr size_t kBatchSize = 10;

    size_t total_verts = 0;
    size_t total_indices = 0;
    for (cairns::SceneId sid : scene_ids) {
        Scene::Hot* shot = scenes_pool.GetHot(sid);
        for (cairns::Handle<Mesh> mid : shot->meshes) {
            Mesh::Cold* mcold = meshes_pool.GetCold(mid);
            total_verts += mcold->cpuPositions.size();
            total_indices += mcold->cpuIndices.size();
        }
    }
    if (total_verts == 0) {
        return true;
    }

    const size_t pos_bytes = total_verts * sizeof(glm::vec4);
    const size_t attr_bytes = total_verts * sizeof(VertexAttribute);
    const size_t idx_bytes = total_indices * sizeof(uint32_t);

    BufferDesc vd{};
    vd.byte_size = static_cast<uint32_t>(pos_bytes + attr_bytes);
    vd.usage = kUsageVertex | kUsageIndex;
    vd.memory = Memory::kDefault;
    Handle<Buffer> shared_vtx = rm.CreateBuffer(alloc, vd);
    if (shared_vtx.IsNull()) {
        return false;
    }

    BufferDesc id{};
    id.byte_size = static_cast<uint32_t>(idx_bytes);
    id.usage = kUsageVertex | kUsageIndex;
    id.memory = Memory::kDefault;
    Handle<Buffer> shared_idx = rm.CreateBuffer(alloc, id);
    if (shared_idx.IsNull()) {
        return false;
    }

    // Patch primitive offsets to all-GLBs-global ahead of upload so per-batch
    // state stays simple. Walk in the same order as the upload loop.
    size_t running_vert = 0;
    size_t running_idx = 0;
    for (cairns::SceneId sid : scene_ids) {
        Scene::Hot* shot = scenes_pool.GetHot(sid);
        for (cairns::Handle<Mesh> mid : shot->meshes) {
            Mesh::Hot* mhot = meshes_pool.GetHot(mid);
            Mesh::Cold* mcold = meshes_pool.GetCold(mid);
            const int32_t base_vertex = static_cast<int32_t>(running_vert);
            const uint32_t base_index = static_cast<uint32_t>(running_idx);
            for (Primitive& prim : mhot->primitives) {
                prim.vertexOffset += base_vertex;
                prim.firstIndex += base_index;
            }
            running_vert += mcold->cpuPositions.size();
            running_idx += mcold->cpuIndices.size();
        }
    }

    // Upload in batches of kBatchSize scenes. Local vectors fall out of scope
    // at the end of each iteration -> CPU temps released between batches.
    size_t cur_vert_off_bytes = 0;
    size_t cur_attr_off_bytes = 0;
    size_t cur_idx_off_bytes = 0;
    for (size_t batch_start = 0; batch_start < scene_ids.size();
         batch_start += kBatchSize) {
        const size_t batch_end =
            std::min(batch_start + kBatchSize, scene_ids.size());

        std::vector<glm::vec4> pos_batch;
        std::vector<VertexAttribute> attr_batch;
        std::vector<uint32_t> idx_batch;
        for (size_t s = batch_start; s < batch_end; ++s) {
            Scene::Hot* shot = scenes_pool.GetHot(scene_ids[s]);
            for (cairns::Handle<Mesh> mid : shot->meshes) {
                Mesh::Cold* mcold = meshes_pool.GetCold(mid);
                pos_batch.insert(pos_batch.end(), mcold->cpuPositions.begin(),
                                 mcold->cpuPositions.end());
                attr_batch.insert(attr_batch.end(), mcold->cpuAttrs.begin(),
                                  mcold->cpuAttrs.end());
                idx_batch.insert(idx_batch.end(), mcold->cpuIndices.begin(),
                                 mcold->cpuIndices.end());
            }
        }

        const size_t pos_batch_bytes = pos_batch.size() * sizeof(glm::vec4);
        const size_t attr_batch_bytes =
            attr_batch.size() * sizeof(VertexAttribute);
        const size_t idx_batch_bytes = idx_batch.size() * sizeof(uint32_t);

        rm.UploadBuffer(alloc, shared_vtx,
                        static_cast<uint32_t>(cur_vert_off_bytes),
                        std::span<const uint8_t>(
                            reinterpret_cast<const uint8_t*>(pos_batch.data()),
                            pos_batch_bytes));
        rm.UploadBuffer(alloc, shared_vtx,
                        static_cast<uint32_t>(pos_bytes + cur_attr_off_bytes),
                        std::span<const uint8_t>(
                            reinterpret_cast<const uint8_t*>(attr_batch.data()),
                            attr_batch_bytes));
        rm.UploadBuffer(alloc, shared_idx,
                        static_cast<uint32_t>(cur_idx_off_bytes),
                        std::span<const uint8_t>(
                            reinterpret_cast<const uint8_t*>(idx_batch.data()),
                            idx_batch_bytes));

        cur_vert_off_bytes += pos_batch_bytes;
        cur_attr_off_bytes += attr_batch_bytes;
        cur_idx_off_bytes += idx_batch_bytes;
    }

    Handle<Buffer> attr_alias = rm.buffers.Acquire();
    {
        Buffer::Hot* vh = rm.buffers.GetHot(shared_vtx);
        Buffer::Hot* ah = rm.buffers.GetHot(attr_alias);
        ah->heap_buffer_index = vh->heap_buffer_index;
        ah->offset_in_heap = vh->offset_in_heap + static_cast<uint32_t>(pos_bytes);
    }

    for (cairns::SceneId sid : scene_ids) {
        Scene::Hot* shot = scenes_pool.GetHot(sid);
        for (cairns::Handle<Mesh> mid : shot->meshes) {
            Mesh::Hot* mhot = meshes_pool.GetHot(mid);
            mhot->posHandle = shared_vtx;
            mhot->attrHandle = attr_alias;
            mhot->indexHandle = shared_idx;
        }
    }
    return true;
}

}  // namespace cairns::rhi
