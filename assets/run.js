// Boot workload -- bundled as an asset, loaded by main.cpp + serve_main.cpp
// via RunBootScript on EVERY platform (mac sdl-min, vk windowed, headless
// cairns_serve, ios, android). If this file is missing the asset bundle
// is broken and the app aborts on launch.
//
// Default: 100 GLBs x 5 instantiateGrid passes = 500 actors, the perf
// benchmark workload.
cairns.dispatch("cairns.prefab.loadBatch", { cursor: 0, count: 100 });
for (let k = 0; k < 5; ++k) {
    cairns.dispatch("cairns.scene.instantiateGrid",
                    { first_prefab_idx: 0, prefab_count: 100 });
}
