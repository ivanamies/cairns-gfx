// #224 L9 / #269 headless byte-gate spawn script.
//
// Loads 9 GLBs explicitly (the JS owns the catalog -- no engine-side
// "load everything at boot" anymore), then spawns one entity per loaded
// prefab in a 3x3 grid at z=-3.
//
// Loaded into cairns_serve via:
//   {"op":"cairns.script.eval","args":{"code":"<this file>"}}
// (verify_headless.sh + regenerate_headless_golden.sh do the JSON-escape;
// edit this file, not the inline NDJSON string.)

const CATALOG = [
    "aatrox.glb",
    "aatrox_blood_moon.glb",
    "aatrox_drx.glb",
    "aatrox_justicar.glb",
    "aatrox_lunar_eclipse.glb",
    "aatrox_mecha.glb",
    "aatrox_odyessy.glb",
    "aatrox_prestige_blood_moon.glb",
    "aatrox_prestige_blood_moon2.glb",
];

const SPACING = 4.0 / 3.0;
const START   = -SPACING;
const SCALE   = 0.013 / 3.0;

// Phase 1: load each path. cairns.prefab.load is single-path; the loop
// belongs in the script (per the engine's "engine exposes primitives,
// scripts compose policy" rule).
const prefabIds = [];
for (const path of CATALOG) {
    const r = cairns.dispatch("cairns.prefab.load", { path });
    if (!r.ok || !r.result || !r.result.ok) {
        throw new Error("prefab.load failed: " + path + " -- " +
                        JSON.stringify(r));
    }
    prefabIds.push(r.result.prefab);
}

// Phase 2: spawn one entity per loaded prefab in a 3x3 grid.
for (let i = 0; i < prefabIds.length; i++) {
    const row = (i / 3) | 0;
    const col = i % 3;
    cairns.dispatch("cairns.scene.instantiate", {
        prefab: prefabIds[i],
        x: START + SPACING * col,
        y: START + SPACING * row,
        z: -3,
        scale: SCALE,
        time_phase: i * 0.137,
    });
}
