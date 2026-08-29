// #269 headless byte-gate spawn script.
//
// Replicates the pre-#269 debug-grid: 9 heroes (scene_idx 0..8) in a
// 3x3 grid at z=-3.
//   spacing  = 4 / 3
//   start    = -spacing * (3 - 1) / 2 = -4/3
//   scale    = 0.013 / 3
//   phase[i] = i * 0.137  (so the heroes animate out-of-sync visibly)
//
// Loaded into cairns_serve via:
//   {"op":"cairns.script.eval","args":{"code":"<this file>"}}
// (build_drive.py in verify_headless.sh + regenerate_headless_golden.sh
// does the JSON-escape -- edit this file, not the inline string.)

const SPACING = 4.0 / 3.0;
const START   = -SPACING;
const SCALE   = 0.013 / 3.0;

for (let i = 0; i < 9; i++) {
    const row = (i / 3) | 0;
    const col = i % 3;
    cairns.dispatch("cairns.world.spawnHero", {
        scene_idx: i,
        x: START + SPACING * col,
        y: START + SPACING * row,
        z: -3,
        scale: SCALE,
        time_phase: i * 0.137,
    });
}
