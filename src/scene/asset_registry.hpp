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
#include "util/offset_allocator.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>

namespace cairns {

// Forward-declared to avoid pulling util/gltf_loader.hpp (which pulls
// stb_image's impl) into every TU that just needs AssetId.
struct Scene;

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
        // Non-owning pointer into Engine::scenes_ during the P4 parity
        // window; later commits move ownership into AssetRegistry as
        // unique_ptr<Scene> once AssetRegistry::Load is the real load
        // path. Forward-declared so this header doesn't pull stb_image.
        const Scene* cpu_graph = nullptr;
        // Suballoc slices into the shared packed buffers (deferred; the
        // existing scene_gpu.hpp packs all GLBs into one shared
        // buffer-set today, no per-asset suballoc).
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
    // MatIds globally, refcount = 1, by_key[key] = id. P4 wires the
    // real body; P3-P4 uses RegisterExistingScene below as the seam.
    inline AssetId Load(const std::string& path) { (void)path; return AssetId::Null; }

    // refcount--; at 0, free suballocs + by_key erase + pool.Release().
    // Actual GPU buffer recycle deferred to a later commit (per plan's
    // out-of-scope).
    inline void Release(AssetId id) { (void)id; }

    // P4-only seam: register an already-loaded Scene (still owned by
    // Engine::scenes_) plus its shared GPU buffer handles. Dedup-keyed
    // by scene_index. Later commits replace this with a real Load(path)
    // that owns parsing + suballoc + dedup-by-content-hash.
    inline AssetId RegisterExistingScene(uint32_t scene_idx,
                                         const Scene* scene,
                                         rhi::Handle<rhi::Buffer> pos,
                                         rhi::Handle<rhi::Buffer> attr,
                                         rhi::Handle<rhi::Buffer> index) {
        const uint64_t key = static_cast<uint64_t>(scene_idx);
        if (auto it = by_key_.find(key); it != by_key_.end()) {
            if (auto* c = pool_.GetCold(it->second)) {
                ++c->ref_count;
            }
            return it->second;
        }
        AssetId id = pool_.Acquire();
        // Reused-slot trap (spec §3): freshly re-initialize Cold every
        // Acquire so a previously-released slot doesn't carry over.
        if (auto* cold = pool_.GetCold(id)) {
            *cold = Asset::Cold{};
            cold->cpu_graph = scene;
            cold->ref_count = 1;
        }
        if (auto* hot = pool_.GetHot(id)) {
            hot->pos = pos;
            hot->attr = attr;
            hot->index = index;
        }
        by_key_[key] = id;
        return id;
    }

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
