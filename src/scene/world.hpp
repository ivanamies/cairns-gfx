// scene/world.hpp
//
// A World is a single editable scene: an entt::registry of instances
// plus a root transform. Pooled in ResourceManager<World> on Engine so
// the editor can open >= 4 of them simultaneously, each referenced by a
// generational WorldId.
//
// Cold holds entt::registry via unique_ptr so World::Cold* stays stable
// across ResourceManager<World>::Acquire growth (the cold_ vector may
// move on resize but the pointee doesn't). Belt-and-suspenders: Engine
// also calls worlds_.Reserve(kMaxWorlds) at startup.

#pragma once

#include "core/handle.hpp"

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <cstdint>
#include <memory>

namespace cairns {

struct World {
    struct Hot {
        glm::mat4 root_transform{1.0f};
        bool dirty = true;
        uint32_t proxy_slot = 0;  // index into Engine::world_proxies_
    };

    struct Cold {
        std::unique_ptr<entt::registry> registry =
            std::make_unique<entt::registry>();
        // Future: undo stack, document name, std::vector<AssetId> referenced
        // (for ref-counting cascade on world close).
    };
};

using WorldId = Handle<World>;

}  // namespace cairns
