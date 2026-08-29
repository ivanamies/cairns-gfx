#pragma once

#include "render/render_proxy.hpp"
#include "util/cpu_arena.hpp"

#include <cstdint>
#include <vector>

namespace cairns {

// Per-frame scene proxy bag. `meshes` and `primitives` ride the per-slot
// BumpArena (#219 Chunk B) since they're the only ones the current scene path
// actually pushes into. The remaining six ProxyArray<T> slots are vestigial
// from the #194 plan -- left on default-heap std::vector so we don't churn
// dead code on this PR. When they wake up, convert in place.

// Legacy thin wrapper kept for the unused fields. free_list was never used
// (no Remove call site in the engine); dropped.
template <typename T>
struct ProxyArray {
    std::vector<T> data;

    uint32_t Add(const T& value) {
        data.push_back(value);
        return static_cast<uint32_t>(data.size() - 1);
    }
    void Update(uint32_t idx, const T& value) { data[idx] = value; }
    void Clear() { data.clear(); }
    size_t size() const { return data.size(); }
};

struct RenderProxyArrays {
    cairns::ArenaList<MeshProxy> meshes;
    cairns::ArenaList<PrimitiveProxy> primitives;
    ProxyArray<LineProxy> lines;
    ProxyArray<PointProxy> points;
    ProxyArray<SkinnedAttachment> skins;
    ProxyArray<LightProxy> lights;
    ProxyArray<CameraProxy> cameras;
    ProxyArray<LayerProxy> layers;

    // #219 Chunk B: per-frame bind. Call once at slot Acquire after
    // arena.Reset(). cap_meshes / cap_prims are upper bounds; push_back
    // beyond cap asserts. Sizes are headroom for the 3300-hero benchmark
    // (~3300 meshes / ~11220 primitives) without growing.
    void Reset(cairns::BumpArena& arena,
               uint32_t cap_meshes = 8192,
               uint32_t cap_prims  = 32768) {
        meshes.Reset(arena, cap_meshes);
        primitives.Reset(arena, cap_prims);
        lines.Clear();
        points.Clear();
        skins.Clear();
        lights.Clear();
        cameras.Clear();
        layers.Clear();
    }

    // Default-allocator fallback (no arena bind). Used today only by
    // scene_proxies_[i] when that path is exercised on a non-PerSlot owner.
    // Idempotent; safe to call whether Reset(arena) ran or not.
    void Clear() {
        meshes.clear();
        primitives.clear();
        lines.Clear();
        points.Clear();
        skins.Clear();
        lights.Clear();
        cameras.Clear();
        layers.Clear();
    }
};

}  // namespace cairns
