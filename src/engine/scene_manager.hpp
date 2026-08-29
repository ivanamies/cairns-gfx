// engine/scene_manager.hpp
//
// Scene ownership state, grouped out of the Engine god class (C2 S4): the
// scene pool + asset registry + per-scene proxy arrays + the active/primary/
// secondary scene-id trio. Engine's scene + spawn + extract systems operate on
// it.
//
// [N-node] The active/primary/secondary trio is editor-focus state pretending
// to be structure. The multi-node workflow (N posable scenes at once) wants an
// explicit-scene-first API with `active` as a mere convenience fallback -- that
// API refactor is a follow-up; this commit just corrals the state so the trio
// travels together and is named as the wart it is.

#pragma once

#include <cstdint>
#include <vector>

#include "rhi/resource_manager.hpp"
#include "scene/asset_registry.hpp"
#include "scene/world.hpp"              // Scene, SceneId
#include "render/render_proxy_arrays.hpp"  // RenderProxyArrays

namespace cairns {

struct SceneManager {
    cairns::AssetRegistry assets;
    cairns::ResourceManager<cairns::Scene> pool;
    std::vector<cairns::RenderProxyArrays> proxies;

    // Editor-focus ids. `active` moves as the user switches; `primary` is the
    // index-0 scene (stable); `secondary` backs the multi-scene coexistence test.
    cairns::SceneId active;
    cairns::SceneId secondary;
    cairns::SceneId primary;

    uint64_t next_id = 0;  // #229 M0b: was g_scene_counter (no statics).
};

}  // namespace cairns
