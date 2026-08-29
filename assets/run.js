// Boot workload -- bundled as an asset, loaded by main.cpp + serve_main.cpp
// via RunBootScript on EVERY platform (mac sdl-min, vk windowed, headless
// cairns_serve, ios, android). If this file is missing the asset bundle
// is broken and the app aborts on launch.
//
// 100 GLBs, instantiated cairns.instancePasses times. The engine sets that
// platform-aware: 3 on desktop = 300 actors, 1 on mobile = 100 actors, to fit
// the Adreno/Apple tile budget + the 256 MB skin pool (500 actors = ~288 MB,
// over the WebGPU-portable cap). Falls back to 3 if unset.
cairns.dispatch("cairns.prefab.loadBatch", { cursor: 0, count: 100 });
const passes = (typeof cairns.instancePasses === "number")
    ? cairns.instancePasses : 3;
for (let k = 0; k < passes; ++k) {
    cairns.dispatch("cairns.scene.instantiateGrid",
                    { first_prefab_idx: 0, prefab_count: 100 });
}
