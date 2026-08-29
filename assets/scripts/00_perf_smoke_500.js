// Performance smoke (500 actors): 100 unique GLBs instantiated 5 times.
cairns.dispatch("cairns.prefab.loadBatch", { cursor: 0, count: 100 });
for (let k = 0; k < 5; ++k) {
    cairns.dispatch("cairns.scene.instantiateGrid",
                    { first_prefab_idx: 0, prefab_count: 100 });
}
cairns.dispatch("cairns.particles.enable", { on: true });
