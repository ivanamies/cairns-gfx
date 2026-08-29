// Performance smoke (100 actors): 100 unique GLBs instantiated once.
cairns.dispatch("cairns.prefab.loadBatch", { cursor: 0, count: 100 });
cairns.dispatch("cairns.scene.instantiateGrid",
                { first_prefab_idx: 0, prefab_count: 100 });
