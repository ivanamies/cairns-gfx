// scene/world.hpp  (file name kept for include-stability; the struct is Scene)
//
// A Scene (Unity vocabulary) is a container of GameObjects: an
// entt::registry plus a root transform.
// Pooled in ResourceManager<Scene> on Engine so the editor can open
// >= 4 of them simultaneously, each referenced by a generational
// SceneId. Multi-active by design -- the active_scene_ concept is
// editor focus, never an implicit instantiate target (the Unity
// active-scene wart is in the Refuse bin; see docs/studio_notes.md).
//
// Cold holds entt::registry BY VALUE. Pointer stability across
// ResourceManager<Scene>::Acquire growth is guaranteed by
// scenes_.Reserve(kMaxScenes) at engine startup -- the cold_ vector
// never reallocates, so Scene::Cold* and registry-internal pointers
// stay valid for the engine's lifetime. (entt::registry itself is
// move-constructible, so it works inside the pool's storage even
// before Reserve, but we rely on Reserve to keep external Cold*
// callers safe.)

#pragma once

#include "core/handle.hpp"

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <cstdint>

namespace cairns {

struct Scene {
    // The entt registry is parameterized with the block allocator (its own
    // quarantined kRegionEcs region). The null-arena default ctor falls back
    // to malloc until re-seated onto cpu_block_ at scene Acquire
    // (Engine::ReseatSceneOnBlock).
    using Registry = entt::basic_registry<entt::entity,
                                          cairns::ChunkStdAllocator<entt::entity>>;

    struct Hot {
        glm::mat4 root_transform{1.0f};
        bool dirty = true;
        uint32_t proxy_slot = 0;  // index into Engine::scene_proxies_
    };

    struct Cold {
        Registry registry;
        // Future: undo stack, document name, std::vector<AssetId> referenced
        // (for ref-counting cascade on world close).
    };
};

using SceneId = Handle<Scene>;

}  // namespace cairns
