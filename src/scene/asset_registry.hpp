// scene/asset_registry.hpp
//
// AssetRegistry is the ONE class wrapper in the scene layer that earns
// its own type (per the planning-Claude "no XRegistry classes" rule).
// It owns four jobs the generic ResourceManager<Asset> can't:
//   1. interning / dedup: by_key maps path/content-hash -> AssetId so a
//      re-loaded GLB returns the same Asset with the same MatIds.
//   2. loading / parsing: Load(path) drives glTF parse + GPU upload.
//   3. cross-resource allocation: OffsetAllocator suballoc slices into
//      shared pos/attr/idx packed buffers.
//   4. cascading lifetime: ref_count drives actual unload.
//
// DEDUP RUNS BEFORE ALLOCATE: a re-loaded GLB MUST return the existing
// AssetId AND existing MatIds. Otherwise material_bind_groups_ drifts
// across reloads and the bind-group resolver table breaks.

#pragma once

#include "core/handle.hpp"
#include "rhi/resource_manager.hpp"
#include "util/gltf_loader.hpp"
#include "util/offset_allocator.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

namespace cairns {

struct Asset {
    struct Hot {
        // Shared GPU buffer handles; same value across every entity that
        // references this asset across every world. Read in every
        // Extract; never duplicated per-world.
        rhi::Handle<rhi::Buffer> pos;
        rhi::Handle<rhi::Buffer> attr;
        rhi::Handle<rhi::Buffer> index;
    };

    struct Cold {
        // Immutable glTF graph (node tree + meshes + materialIds). Held
        // by unique_ptr because Scene has a non-default ctor (takes
        // Arena&) and ResourceManager default-constructs cells.
        std::unique_ptr<Scene> cpu_graph;
        // Suballoc slices into the shared packed buffers (P3 stubs the
        // bodies; P4 wires actual suballoc).
        OffsetAllocator::Allocation pos_alloc{};
        OffsetAllocator::Allocation attr_alloc{};
        OffsetAllocator::Allocation idx_alloc{};
        uint32_t ref_count = 0;
    };
};

using AssetId = Handle<Asset>;

class AssetRegistry {
public:
    // Dedup-before-allocate: by_key lookup first; on hit, refcount++ and
    // return the existing AssetId. On miss: pool.Acquire(), re-init
    // Cold (per the reused-slot trap), parse glTF, suballoc, register
    // MatIds globally, refcount = 1, by_key[key] = id. Body deferred to
    // P4; signature locked in now.
    AssetId Load(const std::string& path);

    // refcount--; at 0, free suballocs + by_key erase + pool.Release().
    // Actual GPU buffer recycle deferred to a later commit (per plan's
    // out-of-scope).
    void Release(AssetId id);

    ResourceManager<Asset>& Pool() { return pool_; }
    const ResourceManager<Asset>& Pool() const { return pool_; }

private:
    ResourceManager<Asset> pool_;
    std::unordered_map<uint64_t, AssetId> by_key_;
    // Shared packed buffer handles owned by the registry (created on
    // first Load). The OffsetAllocator instances carve slices for each
    // Asset's pos/attr/idx range.
    rhi::Handle<rhi::Buffer> shared_pos_;
    rhi::Handle<rhi::Buffer> shared_attr_;
    rhi::Handle<rhi::Buffer> shared_index_;
    OffsetAllocator::Allocator pos_sub_;
    OffsetAllocator::Allocator attr_sub_;
    OffsetAllocator::Allocator idx_sub_;
};

}  // namespace cairns
