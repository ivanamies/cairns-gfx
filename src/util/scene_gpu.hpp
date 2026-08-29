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
// #222 Phase H.4: out_shared_skin returns the per-call shared skin-attrs
// SSBO so the engine can stash it on shared_skin_attrs_buf_ without
// stamping every Mesh::Hot. Null when no scene in scene_ids has any
// skinned mesh.
inline bool LoadScenesGpu(std::span<const cairns::SceneId> scene_ids,
                          cairns::ResourceManager<Scene>& scenes_pool,
                          cairns::ResourceManager<Mesh>& meshes_pool,
                          Resources& rm, Allocator& alloc,
                          Handle<Buffer>* out_shared_skin) {
    static constexpr size_t kBatchSize = 10;

    size_t total_verts = 0;
    size_t total_indices = 0;
    // #221 Skinning Phase 1: total skin verts = sum over SKINNED meshes
    // (cpuSkinAttrs is empty for unskinned meshes). Drives the shared
    // skin-attr storage buffer below.
    size_t total_skin_verts = 0;
    for (cairns::SceneId sid : scene_ids) {
        Scene::Hot* shot = scenes_pool.GetHot(sid);
        for (cairns::Handle<Mesh> mid : shot->meshes) {
            Mesh::Cold* mcold = meshes_pool.GetCold(mid);
            total_verts += mcold->cpuPositions.size();
            total_indices += mcold->cpuIndices.size();
            total_skin_verts += mcold->cpuSkinAttrs.size();
        }
    }
    if (total_verts == 0) {
        return true;
    }

    const size_t pos_bytes = total_verts * sizeof(glm::vec4);
    const size_t attr_bytes = total_verts * sizeof(VertexAttribute);
    const size_t idx_bytes = total_indices * sizeof(uint32_t);
    // #222 Phase S.2: GPU side packs to PackedSkinVertex (8 B) at upload.
    const size_t skin_bytes = total_skin_verts * sizeof(PackedSkinVertex);

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

    // #221 Skinning Phase 1: shared skin-attr SSBO. Storage usage (read by
    // the skin compute kernel as a flat array). Only created when at least
    // one scene actually has a skinned mesh.
    Handle<Buffer> shared_skin;
    if (skin_bytes > 0) {
        BufferDesc sd{};
        sd.byte_size = static_cast<uint32_t>(skin_bytes);
        sd.usage = kUsageStorage;
        sd.memory = Memory::kDefault;
        shared_skin = rm.CreateBuffer(alloc, sd);
        if (shared_skin.IsNull()) {
            return false;
        }
    }

    // Patch primitive offsets to all-GLBs-global ahead of upload so per-batch
    // state stays simple. Walk in the same order as the upload loop.
    // #221 Skinning F5: snapshot running_vert into Mesh::Hot::global_base_vertex
    // so skinned draws can recover the mesh-local baseVertex
    // (prim.vertexOffset - global_base_vertex). Track skin verts separately
    // since unskinned meshes don't contribute.
    size_t running_vert = 0;
    size_t running_idx = 0;
    size_t running_skin_vert = 0;
    for (cairns::SceneId sid : scene_ids) {
        Scene::Hot* shot = scenes_pool.GetHot(sid);
        for (cairns::Handle<Mesh> mid : shot->meshes) {
            Mesh::Hot* mhot = meshes_pool.GetHot(mid);
            Mesh::Cold* mcold = meshes_pool.GetCold(mid);
            const int32_t base_vertex = static_cast<int32_t>(running_vert);
            const uint32_t base_index = static_cast<uint32_t>(running_idx);
            mhot->global_base_vertex = static_cast<uint32_t>(running_vert);
            mhot->skin_attr_base_vertex =
                static_cast<uint32_t>(running_skin_vert);
            mhot->vert_count =
                static_cast<uint32_t>(mcold->cpuPositions.size());
            for (Primitive& prim : mhot->primitives) {
                prim.vertexOffset += base_vertex;
                prim.firstIndex += base_index;
            }
            running_vert += mcold->cpuPositions.size();
            running_idx += mcold->cpuIndices.size();
            running_skin_vert += mcold->cpuSkinAttrs.size();
        }
    }

    // Upload in batches of kBatchSize scenes. Local vectors fall out of scope
    // at the end of each iteration -> CPU temps released between batches.
    // #221 Skinning Phase 1: skin attrs (when present) batch alongside the
    // existing pos/attr/idx streams.
    size_t cur_vert_off_bytes = 0;
    size_t cur_attr_off_bytes = 0;
    size_t cur_idx_off_bytes = 0;
    size_t cur_skin_off_bytes = 0;
    for (size_t batch_start = 0; batch_start < scene_ids.size();
         batch_start += kBatchSize) {
        const size_t batch_end =
            std::min(batch_start + kBatchSize, scene_ids.size());

        std::vector<glm::vec4> pos_batch;
        std::vector<VertexAttribute> attr_batch;
        std::vector<uint32_t> idx_batch;
        std::vector<PackedSkinVertex> skin_batch;
        std::vector<SkinVertex> skin_raw;
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
                // #222 Phase S.2: pack u8 joints + u8 unorm weights with
                // largest-remainder renorm at upload.
                for (const SkinVertex& sv : mcold->cpuSkinAttrs) {
                    skin_batch.push_back(PackSkinVertex(sv));
                }
            }
        }

        const size_t pos_batch_bytes = pos_batch.size() * sizeof(glm::vec4);
        const size_t attr_batch_bytes =
            attr_batch.size() * sizeof(VertexAttribute);
        const size_t idx_batch_bytes = idx_batch.size() * sizeof(uint32_t);
        const size_t skin_batch_bytes = skin_batch.size() * sizeof(PackedSkinVertex);

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
        if (skin_batch_bytes > 0) {
            rm.UploadBuffer(alloc, shared_skin,
                            static_cast<uint32_t>(cur_skin_off_bytes),
                            std::span<const uint8_t>(
                                reinterpret_cast<const uint8_t*>(skin_batch.data()),
                                skin_batch_bytes));
        }

        cur_vert_off_bytes += pos_batch_bytes;
        cur_attr_off_bytes += attr_batch_bytes;
        cur_idx_off_bytes += idx_batch_bytes;
        cur_skin_off_bytes += skin_batch_bytes;
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
            Mesh::Cold* mcold = meshes_pool.GetCold(mid);
            mhot->posHandle = shared_vtx;
            mhot->attrHandle = attr_alias;
            mhot->indexHandle = shared_idx;
            // #221 Skinning F5: per-skinned-mesh alias of the shared attr
            // region pre-offset by global_base_vertex * sizeof(VertexAttribute).
            // Skinned stream 1 binds this; the recorder's mesh-local
            // vertex_offset then indexes correctly. Same trick as attr_alias
            // above, just per-mesh and at a different offset.
            if (!mcold->cpuSkinAttrs.empty()) {
                Handle<Buffer> skinned_attr_alias = rm.buffers.Acquire();
                Buffer::Hot* vh = rm.buffers.GetHot(shared_vtx);
                Buffer::Hot* sah = rm.buffers.GetHot(skinned_attr_alias);
                sah->heap_buffer_index = vh->heap_buffer_index;
                sah->offset_in_heap =
                    vh->offset_in_heap + static_cast<uint32_t>(pos_bytes) +
                    mhot->global_base_vertex *
                        static_cast<uint32_t>(sizeof(VertexAttribute));
                mhot->attr_skinned_alias = skinned_attr_alias;
            }
        }
    }
    if (out_shared_skin) {
        *out_shared_skin = shared_skin;
    }
    return true;
}

}  // namespace cairns::rhi
