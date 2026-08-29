// scene/world.hpp
//
// A World is a single editable scene: an entt::registry of instances
// plus a root transform. Pooled in ResourceManager<World> on Engine so
// the editor can open >= 4 of them simultaneously, each referenced by a
// generational WorldId.
//
// Cold holds entt::registry BY VALUE. Pointer stability across
// ResourceManager<World>::Acquire growth is guaranteed by
// worlds_.Reserve(kMaxWorlds) at engine startup -- the cold_ vector
// never reallocates, so World::Cold* and registry-internal pointers
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

struct World {
    struct Hot {
        glm::mat4 root_transform{1.0f};
        bool dirty = true;
        uint32_t proxy_slot = 0;  // index into Engine::world_proxies_
    };

    struct Cold {
        entt::registry registry;
        // Future: undo stack, document name, std::vector<AssetId> referenced
        // (for ref-counting cascade on world close).
    };
};

using WorldId = Handle<World>;

}  // namespace cairns
