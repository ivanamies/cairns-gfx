// engine/pick_selection.hpp
//
// Selection + highlight document state and the pick request/result
// handshake. Pure state; Engine's pick + selection systems operate on it.
//
// NOTE: the per-viewport R32U id_target render targets stay on Engine -- they
// are kNumViewports-coupled GPU resources (a render-pipeline concern), not
// selection-document state.

#pragma once

#include <vector>

#include "rhi/resource_manager.hpp"
#include "scene/selection.hpp"  // SelectionType, SelectionTarget

namespace cairns {

// Last resolved pick. The headless facade returns a copy; ConsumePickResult
// reads + clears so each request yields exactly one result.
struct PickResult {
    int viewport = 0;
    uint32_t x = 0;
    uint32_t y = 0;
    cairns::SelectionType type = cairns::SelectionType::kEntity;
    uint32_t id = 0;
    // Picking is a CPU ray-cast (identical on every backend; the browser
    // cannot read back synchronously), so raw == id. The R32U id buffer
    // serves only the GPU-side outline pass.
    uint32_t raw = 0;
};

struct PickSelection {
    // Selection + highlight are document-side; rev counters let the protocol's
    // cairns.selection.changed event know when to emit.
    std::vector<cairns::SelectionTarget> selection;
    std::vector<cairns::SelectionTarget> highlights;
    uint32_t selection_rev = 0;
    uint32_t highlights_rev = 0;

    // Highlights texture: R32U 65x1 packed as [count, id0, id1, ...]. Sampled
    // by outline.frag to filter the edge-detect to the current highlight set.
    // Recreated on highlights_tex_rev change (rare -- clicks).
    rhi::Handle<rhi::Texture> highlights_tex = rhi::Handle<rhi::Texture>::Null;
    uint32_t highlights_tex_rev = 0;

    // Pick request/result: the most recent unresolved (viewport, x, y) click
    // intent, plus the last resolved result.
    bool pending = false;
    int viewport = 0;
    uint32_t x = 0;
    uint32_t y = 0;
    bool resolved = false;
    PickResult last_result;
};

}  // namespace cairns
