// Performance smoke (50 actors): the first 50 unique GLBs of the roster,
// instantiated once. Capped at 50 (not 100) for the web build: 100 unique
// champions preload ~1.2 GB into MEMFS and peak ~3.7 GB at load, OOM-killing the
// whole browser. 50 keeps the MEMFS bundle ~620 MB and well under the tab budget.
cairns.dispatch("cairns.prefab.loadBatch", { cursor: 0, count: 50 });
cairns.dispatch("cairns.scene.instantiateGrid",
                { first_prefab_idx: 0, prefab_count: 50 });
cairns.dispatch("cairns.particles.enable", { on: true });
