// #269 dev-drive scene-manipulation helpers. Loaded once per
// dev_drive.sh start; subsequent commands call spawnTotal(N) etc.
//
// Functions published on globalThis:
//   spawnTotal(N)              -- no-flash: relayout existing, append to N
//   relayout()                 -- rescale + reposition only, no spawn
//   sceneList()                -- {count, byIdx[]}, AABB extent per scene
//   clearAll()                 -- nuke active world (leaks pool slices)
//
// Per-actor scale strategy: each spawned hero is sized so its bind-pose
// bounding-box max extent fills 0.85 * cell-side. Heterogeneous GLBs
// therefore appear at uniform on-screen size, not raw model scale.

const TARGET_CELL_FRACTION = 0.85;
const Z_PLANE = -3;
const WORLD_SPAN = 4.0;
const PHASE_STRIDE = 0.137;

function _gridDims(N) {
    const grid_n = Math.max(1, Math.ceil(Math.sqrt(N)));
    const spacing = WORLD_SPAN / grid_n;
    const start = -spacing * (grid_n - 1) / 2;
    const cell = spacing * TARGET_CELL_FRACTION;
    return { grid_n, spacing, start, cell };
}

// cairns.dispatch returns the full registry envelope {ok, result, ...}.
// Strip it once so the rest of the helpers read like plain objects.
function _call(op, args) {
    const resp = cairns.dispatch(op, args || {});
    if (resp && resp.ok === false) {
        throw new Error("dispatch " + op + " failed: " +
                         JSON.stringify(resp.error || resp));
    }
    return (resp && resp.result !== undefined) ? resp.result : resp;
}

function _sceneScale(scene_idx, cell) {
    const r = _call("cairns.world.sceneDims", { scene_idx });
    const extent = r.extent_max;
    if (!extent || extent <= 0) {
        // Fallback if bind-pose AABB wasn't captured; matches the
        // pre-#269 uniform-scale heuristic.
        return 0.013 / Math.max(1, Math.round(WORLD_SPAN / cell));
    }
    return cell / extent;
}

globalThis.spawnTotal = function spawnTotal(N) {
    const dims = _gridDims(N);
    const numScenes = _call("cairns.world.numScenes", {}).count;
    if (!numScenes) {
        return { error: "no scenes loaded" };
    }
    const existing = _call("cairns.world.listEntities", {}).entities
                     || [];
    const E = existing.length;
    const target = Math.min(N, 1024);  // kAnimActorsCap

    // Pre-compute per-scene scale so the same scene_idx maps to a
    // stable visual size across calls.
    const sceneScale = new Array(numScenes);
    for (let s = 0; s < numScenes; s++) {
        sceneScale[s] = _sceneScale(s, dims.cell);
    }

    // Reposition + rescale the existing actors first (no flash). We
    // reuse scene_idx = i % numScenes so a given grid slot always shows
    // the same GLB across spawnTotal calls.
    for (let i = 0; i < E && i < target; i++) {
        const row = (i / dims.grid_n) | 0;
        const col = i % dims.grid_n;
        _call("cairns.world.setTransform", {
            entity: existing[i],
            x: dims.start + dims.spacing * col,
            y: dims.start + dims.spacing * row,
            z: Z_PLANE,
            scale: sceneScale[i % numScenes],
        });
    }

    // Append new actors to fill out the grid.
    const spawned = [];
    for (let i = E; i < target; i++) {
        const row = (i / dims.grid_n) | 0;
        const col = i % dims.grid_n;
        const scene_idx = i % numScenes;
        const r = _call("cairns.world.spawnHero", {
            scene_idx,
            x: dims.start + dims.spacing * col,
            y: dims.start + dims.spacing * row,
            z: Z_PLANE,
            scale: sceneScale[scene_idx],
            time_phase: i * PHASE_STRIDE,
        });
        spawned.push({ entity: r.entity || 0, scene_idx });
    }

    return {
        total: target,
        existing: E,
        spawned_now: spawned.length,
        grid_n: dims.grid_n,
        cell: dims.cell,
        clamped: N > 1024,
    };
};

globalThis.relayout = function relayout() {
    const existing = _call("cairns.world.listEntities", {}).entities
                     || [];
    if (existing.length === 0) {
        return { existing: 0 };
    }
    return spawnTotal(existing.length);
};

globalThis.sceneList = function sceneList() {
    const numScenes = _call("cairns.world.numScenes", {}).count;
    const byIdx = [];
    for (let s = 0; s < numScenes; s++) {
        const r = _call("cairns.world.sceneDims", { scene_idx: s });
        byIdx.push({ scene_idx: s, extent_max: r.extent_max });
    }
    return { count: numScenes, byIdx };
};

globalThis.clearAll = function clearAll() {
    return _call("cairns.world.clear", {});
};
