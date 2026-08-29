// engine/prefab_store.hpp
//
// Loaded-prefab store: the prefab/material/mesh generational pools + the
// order-stable parallel vectors (ids/paths/assets), the resident-texture +
// per-batch shared-skin lists, and the loader instruments.
//
// DESTRUCTION ORDER: cpu_block_ + prefab_arena_ deliberately stay on Engine,
// declared BEFORE prefab_store_, so they outlive it -- the pools' ChunkStd
// vectors deallocate into cpu_block_ in ~ResourceManager, and Prefab::Cold's
// interned name slices point into prefab_arena_.

#pragma once

#include <filesystem>
#include <vector>

#include "rhi/resource_manager.hpp"
#include "util/gltf_loader.hpp"       // Prefab, Mesh, Material, PrefabId
#include "util/chunk_allocator.hpp"   // ChunkStdAllocator
#include "util/print_allocator.hpp"   // print_allocator (alloc-trace demo)
#include "util/load_trace.hpp"        // LoadTrace, LoaderCounters, ValidationReport
#include "scene/asset_registry.hpp"   // AssetId

namespace cairns {

// print_allocator tag so CAIRNS_ALLOC_TRACE=1 builds emit a tagged [ALLOC]
// line on every grow of per_batch_shared_skin.
struct kTagPerBatchSharedSkin {
    static constexpr const char* name() {
        return "PrefabStore::per_batch_shared_skin";
    }
};

struct PrefabStore {
    // Prefab::Hot/Cold records live in the pool; prefab_ids is the order-stable
    // parallel list index-by-position consumers iterate.
    cairns::ResourceManager<cairns::Prefab> prefabs;
    std::vector<cairns::PrefabId, cairns::ChunkStdAllocator<cairns::PrefabId>> prefab_ids;
    // Parallel to prefab_ids: GLB path (for the [PICK] log) + registered AssetId.
    std::vector<std::filesystem::path> glb_paths;
    std::vector<cairns::AssetId, cairns::ChunkStdAllocator<cairns::AssetId>> per_prefab_asset;
    // Built once at scene-load; every frame's resident_textures span points here.
    std::vector<rhi::Handle<rhi::Texture>,
                cairns::ChunkStdAllocator<rhi::Handle<rhi::Texture>>>
        resident_textures;
    // Per-batch shared skin-attr SSBO handle (one per LoadPrefabBatch call).
    std::vector<rhi::Handle<rhi::Buffer>,
                cairns::print_allocator<rhi::Handle<rhi::Buffer>,
                                          kTagPerBatchSharedSkin>>
        per_batch_shared_skin;

    // Loader instruments, populated each LoadPrefabBatch call.
    cairns::LoadTrace        last_load_trace{};
    cairns::LoaderCounters   loader_counters{};
    cairns::ValidationReport last_validation_report{};

    // Handle-pilled material + mesh pools (set2 on Material::Hot; meshes lifted
    // out of Scene so multiple scenes can share one loaded GLB via AssetRegistry).
    cairns::ResourceManager<cairns::Material> materials;
    cairns::ResourceManager<cairns::Mesh> meshes;
};

}  // namespace cairns
