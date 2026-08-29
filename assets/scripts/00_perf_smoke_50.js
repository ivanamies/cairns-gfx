// Performance smoke (50 actors): the first 50 unique GLBs of the roster,
// instantiated once. Capped at 50 (not 100) for the web build -- champion GLBs
// are lazy-fetched over HTTP (NOT bundled in MEMFS), but 100 unique residents
// still peak the tab's memory + GPU budget at load. 50 keeps the load peak well
// under the browser's limit.
cairns.dispatch("cairns.prefab.loadBatch", { cursor: 0, count: 50 });
cairns.dispatch("cairns.scene.instantiateGrid",
                { first_prefab_idx: 0, prefab_count: 50 });
