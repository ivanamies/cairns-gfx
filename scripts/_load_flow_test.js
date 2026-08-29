// #224 verify_load_flow: the user's stated sequence, verified at EVERY step.
//
//   step 1: engine starts empty           assert count == 0
//   step 2: particles are running         dump tmp/_lf_empty.png (particles
//                                          + background; the wrapper asserts
//                                          file exists AND is not blank)
//   step 3: load 3 GLBs                   assert count == 3
//   step 4: snapshot Mesh::Hot handles    (L6 baseline)
//   step 5: load 3 MORE                   assert count == 6
//   step 6: append-only contract          assert 0 mismatches in the snapshot
//   step 7: instantiate ALL 6 prefabs     assert listEntities.count == 6
//   step 8: render frames + dump          tmp/_lf_loaded.png ; wrapper
//                                          asserts != tmp/_lf_empty.png
//                                          (entities CHANGED the image)
//   step 9: "animate all" -- the JS loops one cairns.render.frame at a
//           time and we assert that animation drives a per-frame pose
//           delta. dump tmp/_lf_animated.png ; wrapper asserts !=
//           tmp/_lf_loaded.png (poses moved between frame ticks)
//
// Each `run()` step appends to the results { ok, fail, errors[] }.
// The shell wrapper (verify_load_flow.sh) checks the JS return AND the
// dump-file deltas. Skipping any verify is the bug we are guarding
// against (see MISTAKES.md "Committed without verifying").

const BATCH_A = [
    "aatrox.glb",
    "aatrox_blood_moon.glb",
    "aatrox_drx.glb",
];
const BATCH_B = [
    "ahri.glb",
    "ahri_academy.glb",
    "ahri_arcade.glb",
];

const results = { ok: 0, fail: 0, errors: [] };
function run(name, fn) {
    try { fn(); results.ok++; }
    catch (e) {
        results.fail++;
        results.errors.push(name + ": " + (e.message || e));
    }
}
function _call(op, args) {
    const r = cairns.dispatch(op, args || {});
    if (!r || r.ok === false) {
        throw new Error("dispatch " + op + " failed: " +
                        JSON.stringify(r.error || r));
    }
    return r.result;
}
function _count() { return _call("cairns.prefab.count").count; }
function _ticks(n) {
    for (let i = 0; i < n; i++) {
        cairns.dispatch("cairns.render.frame", {});
    }
}
function _dump(path) {
    return _call("cairns.io.dumpTexture", { target: "final", path });
}
function _assertInvariants(where) {
    const r = _call("cairns.debug.checkInvariants");
    if (r.violations !== 0) {
        throw new Error("invariant violation @ " + where + " (" +
                        r.violations + "): " +
                        JSON.stringify(r.messages || []));
    }
}
// The shell sets globalThis.LF_BACKEND ("metal" or "vk") via eval prelude
// so dump paths don't collide between backends.
const BK = (typeof globalThis.LF_BACKEND === "string")
            ? globalThis.LF_BACKEND : "x";
const P_EMPTY    = "tmp/_lf_" + BK + "_empty.png";
const P_LOADED   = "tmp/_lf_" + BK + "_loaded.png";
const P_ANIMATED = "tmp/_lf_" + BK + "_animated.png";

// ── step 1 ────────────────────────────────────────────────────────────
run("step 1: engine starts empty", () => {
    const c = _count();
    if (c !== 0) {
        throw new Error("expected count=0 at boot, got " + c);
    }
});

// ── step 2 ────────────────────────────────────────────────────────────
run("step 2: particles render at boot (no GLBs loaded)", () => {
    _ticks(5);
    const r = _dump(P_EMPTY);
    if (r.path !== P_EMPTY) {
        throw new Error("empty-state dump failed: " + JSON.stringify(r));
    }
});

// ── step 3 ────────────────────────────────────────────────────────────
run("step 3: load 3 paths -> count becomes 3", () => {
    for (const p of BATCH_A) {
        const r = _call("cairns.prefab.load", { path: p });
        if (!r.ok) {
            throw new Error("load failed for " + p + ": " + JSON.stringify(r));
        }
    }
    const c = _count();
    if (c !== 3) {
        throw new Error("expected count=3 after batch A, got " + c);
    }
    _assertInvariants("after batch A load");
});

// ── step 4 ────────────────────────────────────────────────────────────
run("step 4: snapshot Mesh::Hot handles", () => {
    const r = _call("cairns.debug.snapshotPrefabHandles");
    if (typeof r.snapshot_size !== "number" || r.snapshot_size === 0) {
        throw new Error("snapshot returned empty: " + JSON.stringify(r));
    }
});

// ── step 5 ────────────────────────────────────────────────────────────
run("step 5: load 3 MORE paths -> count becomes 6", () => {
    for (const p of BATCH_B) {
        const r = _call("cairns.prefab.load", { path: p });
        if (!r.ok) {
            throw new Error("load failed for " + p + ": " + JSON.stringify(r));
        }
    }
    const c = _count();
    if (c !== 6) {
        throw new Error("expected count=6 after batch B, got " + c);
    }
    _assertInvariants("after batch B load");
});

// ── step 6 ────────────────────────────────────────────────────────────
run("step 6: first 3 prefabs' handles unchanged (APPEND-only)", () => {
    const r = _call("cairns.debug.assertAppendOnly");
    if (r.mismatches !== 0) {
        throw new Error("APPEND-only contract violated: " + r.mismatches +
                        " mismatch(es) -- batch B mutated batch A's handles");
    }
});

// ── step 7 ────────────────────────────────────────────────────────────
run("step 7: instantiate all 6 prefabs; listEntities.count == 6", () => {
    const n_total = BATCH_A.length + BATCH_B.length;
    // Layout: 6 heroes in a horizontal row at the same depth+scale
    // the verify_headless byte-gate uses (z=-3, scale 0.00433 = the
    // pre-#269 grid_n=3 normalizer). That combo is the ONE rendering
    // path the vk backend is provably-good on; load_flow piggybacking
    // it dodges a pre-existing vk depth/scale-sensitive render bug
    // that surfaces at z=-5,scale=0.002 (the bug is real but separate
    // from this harness's #228 work; tracked via the runtime-load
    // golden in H5).
    const SCALE = 0.00433;
    const SPACING = 1.333;
    const START = -SPACING * (n_total - 1) * 0.5;
    for (let i = 0; i < n_total; i++) {
        _call("cairns.scene.instantiate", {
            prefab: i,
            x: START + i * SPACING, y: 0, z: -3,
            scale: SCALE,
            time_phase: i * 0.137,
        });
    }
    const r = _call("cairns.scene.listEntities");
    if (r.count !== n_total) {
        throw new Error("expected " + n_total +
                        " entities after instantiate, got " + r.count);
    }
    _assertInvariants("after instantiate");
});

// ── step 8 ────────────────────────────────────────────────────────────
run("step 8: render frames + dump; entities changed the image", () => {
    _ticks(10);
    const r = _dump(P_LOADED);
    if (r.path !== P_LOADED) {
        throw new Error("loaded-state dump failed: " + JSON.stringify(r));
    }
});

// ── step 9 ────────────────────────────────────────────────────────────
run("step 9: animate all -- tick frames; poses move between dumps", () => {
    // Advance the FixedClock by enough sim steps that the walking-clip
    // sampler picks a noticeably different pose. The clip sampler is
    // continuous in sim time; ~30 ticks at FixedClock's 16.67ms step is
    // ~0.5s of animation, well above the per-frame delta noise floor.
    _ticks(30);
    const r = _dump(P_ANIMATED);
    if (r.path !== P_ANIMATED) {
        throw new Error("animated-state dump failed: " + JSON.stringify(r));
    }
});

JSON.stringify(results);
