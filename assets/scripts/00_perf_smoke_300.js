// Performance smoke (300 actors): 100 unique GLBs instantiated 3 times.
// Was 500; the WebGPU-portable 256 MB skin pool only fits 300 actors (400 =
// ~285 MB, measured headless 2026-06-22, exhausts the pool). See PERFORMANCE.md.
cairns.dispatch("cairns.prefab.loadBatch", { cursor: 0, count: 100 });
for (let k = 0; k < 3; ++k) {
    cairns.dispatch("cairns.scene.instantiateGrid",
                    { first_prefab_idx: 0, prefab_count: 100 });
}
