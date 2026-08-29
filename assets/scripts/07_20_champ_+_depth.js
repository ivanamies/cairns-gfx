// Nested graph: 20 GLBs in resolved color (full frame) + a depthviz strip of
// the same depth buffer along the bottom (render.nestedGraph drives the
// composite). Single camera.
cairns.dispatch("cairns.prefab.loadBatch", { cursor: 0, count: 20 });
cairns.dispatch("cairns.scene.instantiateGrid",
                { first_prefab_idx: 0, prefab_count: 20 });
cairns.dispatch("cairns.render.nestedGraph", { on: true });
