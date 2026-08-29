#pragma once

#include "rhi/resource_manager.hpp"
#include "scene/scene_entity.hpp"
#include "util/gltf_loader.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

namespace cairns {

// Entities go through allocator D -- generational handles via ResourceManager.
// live_entities is the packed live list the Extract hot path walks (so the walk
// is dense even though the Hot/Cold arrays may have holes after Release).
struct SceneWorld {
    rhi::ResourceManager<SceneEntity> entities;
    std::vector<rhi::Handle<SceneEntity>> live_entities;
    const Scene* scenes = nullptr;
    size_t scene_count = 0;
    glm::mat4 root_transform = glm::mat4(1.0f);

    rhi::Handle<SceneEntity> AddEntity(const SceneEntity::Hot& hot) {
        const rhi::Handle<SceneEntity> h = entities.Acquire();
        *entities.GetHot(h) = hot;
        live_entities.push_back(h);
        return h;
    }

    void RemoveEntity(rhi::Handle<SceneEntity> h) {
        entities.Release(h);
        for (size_t i = 0; i < live_entities.size(); ++i) {
            if (live_entities[i].index == h.index &&
                live_entities[i].generation == h.generation) {
                live_entities[i] = live_entities.back();
                live_entities.pop_back();
                break;
            }
        }
    }

    void ClearEntities() {
        entities.Reset();
        live_entities.clear();
    }
};

}  // namespace cairns
