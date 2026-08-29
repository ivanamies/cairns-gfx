#pragma once

#include "render/render_proxy.hpp"
#include "util/cpu_arena.hpp"

#include <cstdint>

namespace cairns {

// Per-frame scene proxy bag. `meshes` and `primitives` ride the per-slot
// BumpArena -- the only proxy kinds the current scene path pushes into.
// New proxy kinds must be ArenaList<T> too, or they escape the block hash.

struct RenderProxyArrays {
    cairns::ArenaList<MeshProxy> meshes;
    cairns::ArenaList<PrimitiveProxy> primitives;

    // Per-frame bind. Call once at slot Acquire after arena.Reset().
    // cap_meshes / cap_prims are upper bounds; push_back beyond cap
    // asserts. Sizes are headroom for the 3300-hero benchmark
    // (~3300 meshes / ~11220 primitives) without growing.
    void Reset(cairns::BumpArena& arena,
               uint32_t cap_meshes = 8192,
               uint32_t cap_prims  = 32768) {
        meshes.Reset(arena, cap_meshes);
        primitives.Reset(arena, cap_prims);
    }

    // Default-allocator fallback (no arena bind). Used today only by
    // scene_proxies_[i] when that path is exercised on a non-PerSlot owner.
    // Idempotent; safe to call whether Reset(arena) ran or not.
    void Clear() {
        meshes.clear();
        primitives.clear();
    }
};

}  // namespace cairns
