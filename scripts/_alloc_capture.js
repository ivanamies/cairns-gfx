// Minimal workload for [ALLOC] capture.
// Steps (fences are emitted by Engine itself):
//   1. load 3 GLBs
//   2. instantiate all 3
//   3. render 30 frames
//   4. reload prefab 0
//   5. render 30 frames
//   6. unloadAll
//   7. render 5 frames

const PATHS = ["aatrox.glb", "ahri.glb", "aatrox_blood_moon.glb"];

function _call(op, args) {
    const r = cairns.dispatch(op, args || {});
    if (!r || r.ok === false) {
        throw new Error("dispatch " + op + " failed: " +
                        JSON.stringify(r.error || r));
    }
    return r.result;
}
function _ticks(n) {
    for (let i = 0; i < n; i++) {
        cairns.dispatch("cairns.render.frame", {});
    }
}

for (const p of PATHS) {
    _call("cairns.prefab.load", { path: p });
}
for (let i = 0; i < PATHS.length; i++) {
    _call("cairns.scene.instantiate", {
        prefab: i,
        x: (i - 1) * 1.5, y: 0, z: -3,
        scale: 0.00433,
    });
}
_ticks(30);
_call("cairns.prefab.reload", { path: PATHS[0] });
_ticks(30);
_call("cairns.scene.clear", {});
_call("cairns.prefab.unloadAll", {});
_ticks(5);

({ ok: true });
